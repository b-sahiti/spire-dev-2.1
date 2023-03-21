/*
 * Prime.
 *     
 * The contents of this file are subject to the Prime Open-Source
 * License, Version 1.0 (the ``License''); you may not use
 * this file except in compliance with the License.  You may obtain a
 * copy of the License at:
 *
 * http://www.dsn.jhu.edu/prime/LICENSE.txt
 *
 * or in the file ``LICENSE.txt'' found in this distribution.
 *
 * Software distributed under the License is distributed on an AS IS basis, 
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License 
 * for the specific language governing rights and limitations under the 
 * License.
 *
 * Creators:
 *   Yair Amir            yairamir@cs.jhu.edu
 *   Jonathan Kirsch      jak@cs.jhu.edu
 *   John Lane            johnlane@cs.jhu.edu
 *   Marco Platania       platania@cs.jhu.edu
 *   Amy Babay            babay@cs.jhu.edu
 *   Thomas Tantillo      tantillo@cs.jhu.edu
 *
 * Major Contributors:
 *   Brian Coan           Design of the Prime algorithm
 *   Jeff Seibert         View Change protocol
 *      
 * Copyright (c) 2008 - 2017
 * The Johns Hopkins University.
 * All rights reserved.
 * 
 * Partial funding for Prime research was provided by the Defense Advanced 
 * Research Projects Agency (DARPA) and the National Science Foundation (NSF).
 * Prime is not necessarily endorsed by DARPA or the NSF.  
 *
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "arch.h"
#include "spu_alarm.h"
#include "spu_memory.h"
#include "data_structs.h"
#include "network.h"
#include "utility.h"
#include "pre_order.h"
#include "error_wrapper.h"
#include "packets.h"
#include "process.h"
#include "order.h"
#include "signature.h"
#include "recon.h"
#include "validate.h"

/* Globally Accessible Variables */
extern server_variables    VAR;
extern network_variables   NET;
extern server_data_struct  DATA;
extern benchmark_struct    BENCH;

/* Local Functions */
//int32u PRE_ORDER_Proof_ARU (int32u server, po_aru_signed_message *proof);
void   PRE_ORDER_Periodically(int dummy, void *dummyp);
void   PRE_ORDER_Create_TAT_Entry(void);
void   PRE_ORDER_Periodic_Retrans(int d1, void *d2);

void PRE_ORDER_Periodically(int function_num, void *dummyp)
{
  sp_time t;

  /*SIG_Attempt_To_Generate_PO_Messages();*/

  if (function_num == 0) {
    if(SEND_PO_ACKS_PERIODICALLY)
      PRE_ORDER_Send_PO_Ack();
  } else if (function_num == 1) {
    if(SEND_PO_ARU_PERIODICALLY)
      PRE_ORDER_Send_PO_ARU();
  } else {
    if(!UTIL_I_Am_Leader())
      PRE_ORDER_Send_Proof_Matrix();
  }

  /* Re-schedule the event for next time */
  t.sec  = PO_PERIODICALLY_SEC;
  t.usec = PO_PERIODICALLY_USEC;
  E_queue(PRE_ORDER_Periodically, (function_num + 1) % 3, NULL, t);
}

void PRE_ORDER_Periodic_Retrans(int d1, void *d2)
{
    po_slot *p_slot;
    stdit it;
    sp_time t;
    int32u i;
    int32u more_to_ack;
    signed_message *ack, *po_aru;

    /* Retransmit any of my PO_Requests that have yet to be executed */
    i = VAR.My_Server_ID;
    stdhash_begin(&DATA.PO.History[i], &it);
    while (!stdhash_is_end(&DATA.PO.History[i], &it))
    {
        p_slot = *(po_slot **)stdit_val(&it);

        /* This is a po-request I sent that didn't get enough acks yet */
        if (PRE_ORDER_Seq_Compare(p_slot->seq, DATA.PO.last_executed_po_reqs[VAR.My_Server_ID]) > 0) {
            Alarm(DEBUG, "Retransmitting %u %u %x\n", p_slot->seq.incarnation, p_slot->seq.seq_num, p_slot->po_request);
            UTIL_Broadcast(p_slot->po_request);
        }

        stdit_next(&it);
    }

    /* Retransmit PO-Ack if the PO-Request isn't executed yet */
    PRE_ORDER_Update_ARU();
    more_to_ack = 1;
    while(more_to_ack) {
      /* Now construct the Local PO_Ack */
      ack = PRE_ORDER_Construct_PO_Ack(&more_to_ack, 1);
    
      /* Ack may be NULL if there is no ack to send right now */
      if (ack == NULL)
        break;

      SIG_Add_To_Pending_Messages(ack, BROADCAST, UTIL_Get_Timeliness(PO_ACK));
      dec_ref_cnt(ack);
    }
  
    /* Retransmit my latest PO_ARU each time */
    po_aru = PRE_ORDER_Construct_PO_ARU();

    /* Print vector */
    /* po_aru_message *po_aru_specific = (po_aru_message *)(po_aru + 1);
    printf("PO ARU: [ ");
    for (i = 0; i < NUM_SERVERS; i++) {
        printf("(%u,%u) ", po_aru_specific->ack_for_server[i].incarnation, po_aru_specific->ack_for_server[i].seq_num);
    }
    printf("]\n"); */

    UTIL_RSA_Sign_Message(po_aru); //need messages to be compact, so no merkle tree stuff...
    PRE_ORDER_Process_PO_ARU(po_aru); //broadcast doesn't send it to myself, so apply to datastructs
    UTIL_Broadcast(po_aru);
    dec_ref_cnt(po_aru);

    t.sec  = RETRANS_PERIOD_SEC;
    t.usec = RETRANS_PERIOD_USEC;
    E_queue(PRE_ORDER_Periodic_Retrans, 0, NULL, t);
}

void PRE_ORDER_Process_Update(signed_message *update)
{
  update_message *up_specific;
  signed_message *payload;
  po_seq_pair ps;

  /* Check if this update from this client is old */
  up_specific = (update_message*)(update+1);
  payload = (signed_message *)(up_specific + 1);

  Alarm(DEBUG, "PO_Process_Update: [%d,%d,%d]\n", up_specific->server_id, 
                    up_specific->incarnation, up_specific->seq_num);

  if (update->machine_id == VAR.My_Server_ID) {
    if (up_specific->incarnation != 0 || up_specific->seq_num != 0)
        return;

    ps = DATA.PO.intro_client_seq[VAR.My_Server_ID];
    ps.seq_num++;
    up_specific->incarnation = ps.incarnation;
    up_specific->seq_num = ps.seq_num;
    UTIL_RSA_Sign_Message(update);
  }
  else {
    ps.incarnation = up_specific->incarnation;
    ps.seq_num = up_specific->seq_num;
  }

  if (PRE_ORDER_Seq_Compare(ps, DATA.PO.intro_client_seq[up_specific->server_id]) <= 0) 
  {
    Alarm(PRINT, "Duplicate client message [%d,%d,%d]\n", up_specific->server_id, 
            up_specific->incarnation, up_specific->seq_num);
    return;
  }

  DATA.PO.intro_client_seq[up_specific->server_id] = ps;

  /* Add the update to the outgoing po_request list  */
  UTIL_DLL_Add_Data(&DATA.PO.po_request_dll, update);

  /* Timing PO Duration */
  //DATA.PO.already_timed = 0;
  //UTIL_Stopwatch_Start(&DATA.PO.po_duration_sw);
    
  /* If we're not sending PO-Requests periodically, try to send one
   * right away. */
  if(!SEND_PO_REQUESTS_PERIODICALLY)
    PRE_ORDER_Send_PO_Request();
}

void PRE_ORDER_Send_PO_Request()
{
  signed_message *po_request;
  double time;
  int32u dest_bits;
  int32u counter = 0;

  /* If we send PO-Requests periodically, make sure it's been long
   * enough since we last sent one. */
  if(SEND_PO_REQUESTS_PERIODICALLY) {
    UTIL_Stopwatch_Stop(&DATA.PO.po_request_sw);
    time = UTIL_Stopwatch_Elapsed(&DATA.PO.po_request_sw);
    if(time < ((PO_PERIODICALLY_USEC / 1000000.0) * 
	       PO_REQUEST_PERIOD))
      return;
  }

  while(!UTIL_DLL_Is_Empty(&DATA.PO.po_request_dll)) {

    /* Check if we reached the MAX number PO requests in flight, and if so
     * stop reading in new client requests */
    if (DATA.PO.po_seq.seq_num - DATA.PO.po_seq_executed.seq_num >= MAX_PO_IN_FLIGHT) {
        E_detach_fd(NET.from_client_sd, READ_FD);
        Alarm(DEBUG, "Detaching client fd\n");
        break;
    }

    /* Build a new PO_Request */
    po_request = PRE_ORDER_Construct_PO_Request();

    /* Broadcast by default.  Only changed if there is a RECON_ATTACK. */
    dest_bits = BROADCAST;
    
    /* Recon attack: */
    if(UTIL_I_Am_Faulty()) {
      /* Set the destination bits to everyone except server 4 */
      int32u i;
      for(i = 1; i <= NUM_SERVERS; i++) {
        if(i != VAR.My_Server_ID && i < 4)
        UTIL_Bitmap_Set(&dest_bits, i);
      }
    }

    /* Add it to the list of messages to be signed*/
    Alarm(DEBUG, "Adding PO (%d, %d, %d) to pending messages\n", 
        po_request->machine_id, 
        ((po_request_message *)(po_request + 1))->seq.incarnation,
        ((po_request_message *)(po_request + 1))->seq.seq_num);
    SIG_Add_To_Pending_Messages(po_request, dest_bits, 
				UTIL_Get_Timeliness(PO_REQUEST));
    dec_ref_cnt(po_request);
    counter++;

    /* Sanity check.  This indicates an infinite loop. Should never happen. */
    if(counter == 500) {
      Alarm(PRINT, "Length of po_request_dll is %d\n", 
	    DATA.PO.po_request_dll.length);
      Alarm(PRINT, "DATA.PO.po_seq_num = %d\n", DATA.PO.po_seq.seq_num);
      assert(0);
    }
  }

  if(counter > 0)
    Alarm(DEBUG, "Batched %d local PO-Requests\n", counter);

  /* If we sent one, don't do it again for a little while */
  if(SEND_PO_REQUESTS_PERIODICALLY && (counter > 0))
    UTIL_Stopwatch_Start(&DATA.PO.po_request_sw);
}

void PRE_ORDER_Process_PO_Request(signed_message *po_request)
{
  po_slot *slot;
  po_request_message *po_request_specific;
  int32u id; //, seq_num;
  po_seq_pair ps;
  stdit it;
  
  /* Get the po slot for this message and store the po_request in this slot */
  po_request_specific = (po_request_message*)(po_request+1);

  /* If we've already garbage collected this slot, don't do anything */
  if(PRE_ORDER_Seq_Compare(po_request_specific->seq, 
        DATA.PO.white_line[po_request->machine_id]) <= 0) 
  {
    Alarm(DEBUG, "Discarding PO-Request %d %d %d, already gc\n",
      po_request->machine_id, po_request_specific->seq.incarnation,
      po_request_specific->seq.seq_num);
    return;
  }

  assert((po_request->machine_id >= 1) &&
     (po_request->machine_id <= NUM_SERVERS));

  /* Special Check: For new incarnation of this machine, we want to 
   * update our own data structures to reflect the incarnation change
   * and be ready to start ACKing po_requests from this new one */
  ps.incarnation = po_request_specific->seq.incarnation;
  ps.seq_num = 0;
  if (ps.incarnation > DATA.PO.aru[po_request->machine_id].incarnation) {
    /* printf("  PO_Request from %u with new incarnation = %u, old = %u\n",
            po_request->machine_id,
            ps.incarnation, DATA.PO.aru[po_request->machine_id].incarnation); */
    DATA.PO.max_acked[po_request->machine_id] = ps;
    DATA.PO.aru[po_request->machine_id] = ps;
    DATA.PO.cum_aru[po_request->machine_id] = ps;
    /* MAYBE - update my own cum_max_acked, although should be filled in
        by processing my own PO_Ack */
  }

  slot = UTIL_Get_PO_Slot(po_request->machine_id, po_request_specific->seq);

  /* If we already have this po request, don't do anything */
  if(slot->po_request) {
    Alarm(DEBUG, "Discarding PO-Request %d %d %d, already have it.\n",
      po_request->machine_id, po_request_specific->seq.incarnation,
      po_request_specific->seq.seq_num);
    return;
  }

  /* Store the po_request if we need it. */
  inc_ref_cnt(po_request);
  slot->po_request = po_request;
  OPENSSL_RSA_Make_Digest((byte *)po_request, UTIL_Message_Size(po_request), slot->po_request_digest);

  PRE_ORDER_Update_ARU();
  PRE_ORDER_Update_Cum_ARU(po_request->machine_id);

  slot->num_events = po_request_specific->num_events;

  /* See if we were missing this PO-Request when it became eligible for
   * local execution.  If so, mark that we have it.  Then, if this means
   * we can execute the next global sequence number, try. */
  id             = po_request->machine_id;
  ps.incarnation = po_request_specific->seq.incarnation;
  ps.seq_num     = po_request_specific->seq.seq_num;
  stdhash_find(&DATA.PO.Pending_Execution[id], &it, &ps);

  if(!stdhash_is_end(&DATA.PO.Pending_Execution[id], &it)) {
    ord_slot *o_slot;

    o_slot = *((ord_slot **)stdhash_it_val(&it));
    dec_ref_cnt(o_slot);
    stdhash_erase_key(&DATA.PO.Pending_Execution[id], &ps);
    o_slot->num_remaining_for_execution--;

    assert(o_slot->num_remaining_for_execution >= 0);

    Alarm(DEBUG, "Received missing po-request %d %d %d\n", 
            id, ps.incarnation, ps.seq_num);

    if(o_slot->num_remaining_for_execution == 0) {
      sp_time t;
      t.sec = 0; t.usec = 0;
      E_queue(ORDER_Attempt_To_Execute_Pending_Commits, 0, 0, t);
    }

    Alarm(DEBUG, "Filled hole\n");
  }

  if(!SEND_PO_ACKS_PERIODICALLY)
    PRE_ORDER_Send_PO_Ack();
}

void PRE_ORDER_Send_PO_Ack()
{
  signed_message *ack;
  int32u more_to_ack;
  double time;

  /* Make sure we don't send an ack if it hasn't been long enough */
  if(SEND_PO_ACKS_PERIODICALLY) {
    UTIL_Stopwatch_Stop(&DATA.PO.po_ack_sw);
    time = UTIL_Stopwatch_Elapsed(&DATA.PO.po_ack_sw);
    if(time < (PO_PERIODICALLY_USEC / 1000000.0) * PO_ACK_PERIOD)
      return;
  }

  /*  First make sure our local Pre-Order ARU is up to date. */
  PRE_ORDER_Update_ARU();
  
  while(1) {

    /* Now construct the Local PO_Ack */
    ack = PRE_ORDER_Construct_PO_Ack(&more_to_ack, 0);
  
    /* Ack may be NULL if there is no ack to send right now */
    if (ack == NULL)
      break;

    SIG_Add_To_Pending_Messages(ack, BROADCAST, UTIL_Get_Timeliness(PO_ACK));
    dec_ref_cnt(ack);

    if(SEND_PO_ACKS_PERIODICALLY)
      UTIL_Stopwatch_Start(&DATA.PO.po_ack_sw);
    
    /* If they tell us there's nothing more to ack, then we're done. */
    if(more_to_ack == 0)
      break;
  }
}

int32u PRE_ORDER_Update_ARU() 
{
  int32u s;
  bool updated = FALSE;
  po_slot *slot;
  po_seq_pair ps;

  /* Attempt to update the pre order aru for each server */
  for (s = 1; s <= NUM_SERVERS; s++) {

    ps = DATA.PO.aru[s];
    ps.seq_num++;
    while((slot = UTIL_Get_PO_Slot_If_Exists(s, ps)) != NULL) {
      if (slot->po_request == NULL) { 
	    /* NULL request -- don't update aru */
	    Alarm(DEBUG,"%d NULL po_request found in slot %d %d srv %d\n",
	            VAR.My_Server_ID, ps.incarnation, ps.seq_num, s);
	    break;
      }

      DATA.PO.aru[s].seq_num++; 
      ps.seq_num++;
      updated = TRUE;
    }
  }
  
  return updated;
}

void PRE_ORDER_Process_PO_Ack(signed_message *po_ack)
{
  po_slot *slot;
  po_ack_message *po_ack_specific;
  po_ack_part *part;
  int32u p;

  /* Iterate over each ack in the aggregate PO-Ack, and apply it to
   * the correct po slot */
  Alarm(DEBUG, "PO_Ack from %d\n", po_ack->machine_id);

  po_ack_specific = (po_ack_message *)(po_ack+1);
  part            = (po_ack_part *)(po_ack_specific+1);

  for (p = 0; p < po_ack_specific->num_ack_parts; p++) {

    /* Mark if I can use this to increase my knowledge of which PO-Requests
     * from originator it has contiguously received and acknowledged. */
    if(PRE_ORDER_Seq_Compare(part[p].seq, 
        DATA.PO.cum_max_acked[po_ack->machine_id][part[p].originator]) > 0)
    {
      Alarm(DEBUG, "Updating cum_max_acked[%u][%u] from %u %u to %u %u\n", 
            po_ack->machine_id, part[p].originator, 
            DATA.PO.cum_max_acked[po_ack->machine_id][part[p].originator].incarnation, 
            DATA.PO.cum_max_acked[po_ack->machine_id][part[p].originator].seq_num, 
            part[p].seq.incarnation, part[p].seq.seq_num);
      DATA.PO.cum_max_acked[po_ack->machine_id][part[p].originator] = part[p].seq;
    }

    /* If we've already garbage collected this slot, don't do anything */
    if(PRE_ORDER_Seq_Compare(part[p].seq, 
            DATA.PO.white_line[part[p].originator]) <= 0) 
    {
      continue;
    }

    slot = UTIL_Get_PO_Slot(part[p].originator, part[p].seq);
    if(!slot->ack_received[po_ack->machine_id]) {
      slot->ack_received[po_ack->machine_id] = 1;
      memcpy(&slot->ack[po_ack->machine_id], &part[p], sizeof(po_ack_part));
      Alarm(DEBUG, "Received ack from %u for %u %u for %u\n", po_ack->machine_id, 
            part[p].seq.incarnation, part[p].seq.seq_num, part[p].originator);
      PRE_ORDER_Update_Cum_ARU(part[p].originator);
    }
  }

  if(!SEND_PO_ARU_PERIODICALLY)
    PRE_ORDER_Send_PO_ARU();
}

void PRE_ORDER_Send_PO_ARU()
{
  signed_message *ack;
  double time;

  /* Make sure it's been long enough since we last sent a PO-ARU */
  if(SEND_PO_ARU_PERIODICALLY) {
    UTIL_Stopwatch_Stop(&DATA.PO.po_aru_sw);
    time = UTIL_Stopwatch_Elapsed(&DATA.PO.po_aru_sw);
    if(time < (PO_PERIODICALLY_USEC / 1000000.0) * (PO_ARU_PERIOD))
      return;
  }

  /* Only send the message if there's something new to report */
  if(DATA.PO.cum_aru_updated == 0)
    return;

  ack = PRE_ORDER_Construct_PO_ARU();
  assert(ack);

  /* int k;
  printf("  PO_ARU: ");
  for (k = 0; k < NUM_SERVERS; k++)
    printf("%d ", ((po_aru_signed_message *)ack)->cum_ack.ack_for_server[k]);
  printf("\n"); */

  //SIG_Add_To_Pending_Messages(ack, BROADCAST, UTIL_Get_Timeliness(PO_ARU));
  UTIL_RSA_Sign_Message(ack); //need messages to be compact, so no merkle tree stuff...
  PRE_ORDER_Process_PO_ARU(ack); //broadcast doesn't send it to myself, so apply to datastructs
  UTIL_Broadcast(ack);
  dec_ref_cnt(ack);

  /* Finished sending latest cum_aru information in this PO_ARU */
  DATA.PO.cum_aru_updated = 0;

  /* Mark that we've just sent one so we don't do it again for awhile */
  UTIL_Stopwatch_Start(&DATA.PO.po_aru_sw);
}

void PRE_ORDER_Update_Cum_ARU(int32u server_id)
{
  int32u s, i, ack_count;
  po_slot *slot;
  po_seq_pair ps;

  /* Attempt to update the pre order cumulative aru for server_id s */
  s = server_id;
  
  ps = DATA.PO.cum_aru[s];
  ps.seq_num++;
  while((slot = UTIL_Get_PO_Slot_If_Exists(s, ps))!= NULL) {
      
    /* Make sure we have the PO Request */
    if(slot->po_request == NULL) {
        return;
    }

    ack_count = 0;
    for (i = 1; i <= NUM_SERVERS; i++) {
        
        if (slot->ack_received[i] == 0)
            continue;
    
        if (!OPENSSL_RSA_Digests_Equal(slot->po_request_digest, slot->ack[i].digest))
        {
            Alarm(PRINT, "DIGEST ERROR (Update_Cum_ARU: %u %u %u %u)\n", ps.incarnation, 
                        ps.seq_num, i, s);
            continue;
        }
    
        ack_count++;
    }

    if(ack_count < (2*NUM_F + NUM_K + 1)) {  /* (n+f)/2 */
        /* not enough acks -- don't update aru */
        return;
    }

    /* Enough acks found for server s*/
    DATA.PO.cum_aru[s].seq_num++; 
    ps.seq_num++;
    DATA.PO.cum_aru_updated = 1;
  }
}

void PRE_ORDER_Process_PO_ARU(signed_message *mess)
{
  //int32u prev_num, num;
  po_aru_signed_message *prev, *cur;
  int32u i;
  char lower, higher;
  po_seq_pair ps;

  Alarm(DEBUG,"Server: %d, PRE_ORDER Received PO-ARU from %d\n",
        VAR.My_Server_ID, mess->machine_id );

  /* If the PO_ARU is contained in a Proof matrix, then it may be a null
   * vector.  Don't apply it in this case. */
  if(mess->type != PO_ARU)
    return;

  /* We will store the latest PO-ARU received from each server -- this
   * constitutes the proof */
  Alarm(DEBUG, "PO_ARU from %d\n", mess->machine_id);

  /* ESTCP - Potentially need separate storage space and flag for 
   * last stored PO_ARU if you roll a replica with new incarnation
   * back after an ordering/execution - since the rollback is not
   * actually signed. In the meantime, still send out the latest
   * signed thing you have from him when you send out Proof_Matrix */

  cur = (po_aru_signed_message *)mess;
  prev = &(DATA.PO.cum_acks[mess->machine_id]);

  /* Ignore old PO_ARUs */
  /* num      = ((po_aru_message*)(mess+1))->num;
  prev_num = prev->cum_ack.num;

  if (num <= prev_num)
    return;  */

  /* Compare this PO_ARU with the last one I have stored. If its a new incarnation,
   * its automatically higher. If its an old incarnation, automatically lower. 
   * Otherwise, check for any inconsistencies, blacklist if so */
  /* TODO: need to be careful that a bad replica doesn't block future incarnations
   * of himself by sending a PO_Request that is very far ahead in time. We can
   * handle this with a high watermark, aka limiting how far ahead his clock
   * could actually be of mine. Then its true the bad replica can block, but
   * only up to that fixed spot ahead in time. */
  lower = higher = 0;
  if (cur->cum_ack.incarnation < prev->cum_ack.incarnation) {
    lower = 1;
  }
  else {
    for (i = 0; i < NUM_SERVERS; i++) {
      if (PRE_ORDER_Seq_Compare(cur->cum_ack.ack_for_server[i], 
            prev->cum_ack.ack_for_server[i]) < 0) 
      {
        lower = 1;
      }
      else if (PRE_ORDER_Seq_Compare(cur->cum_ack.ack_for_server[i], 
            prev->cum_ack.ack_for_server[i]) > 0) 
      {
        higher = 1;
      }
    }
  }

  if (higher && lower) {
    if (cur->cum_ack.incarnation > prev->cum_ack.incarnation) {
      Alarm(PRINT, "Process_PO_ARU: new incarnation for %u, must catch up\n", 
                mess->machine_id);
    }
    else {
      Alarm(PRINT, "Process_PO_ARU: INCONSISTENT PO_ARU from %d. Blacklist!\n", 
             mess->machine_id);
      /* Blacklist(mess->machine_id); */
    }
    return;
  }
  else if (lower || (!lower && !higher)) {
    return;
  }
  else if (higher && UTIL_I_Am_Leader()) {
      DATA.ORD.should_send_pp = 1;
  }
    
  memcpy( (void*)( &DATA.PO.cum_acks[mess->machine_id]),
        (void*)mess, sizeof(po_aru_signed_message));
  
  /* if (DATA.PO.already_timed == 0) {
    count = 0;
    for (i = 1; i <= NUM_SERVERS; i++) {
      if (DATA.PO.cum_acks[i].cum_ack.ack_for_server[VAR.My_Server_ID-1] == DATA.PO.po_seq_num)
        count++;
    }
    if (count == 2*NUM_F + NUM_K + 1) {
      UTIL_Stopwatch_Stop(&DATA.PO.po_duration_sw);
      DATA.PO.already_timed = 1;
    }
  } */

  /* See if I can use this to increase my knowledge of what the acker
   * has contiguously received with respect to po-requests */
    /* if its a new incarnation, we can't just compare, need to adopt */
  for(i = 1; i <= NUM_SERVERS; i++) {
    ps = cur->cum_ack.ack_for_server[i-1];

    if(PRE_ORDER_Seq_Compare(DATA.PO.cum_max_acked[mess->machine_id][i], ps) < 0)
      DATA.PO.cum_max_acked[mess->machine_id][i] = ps;
  }

  /* If we're not sending the Proof Matrix periodically, then try to
   * send one whenever we receive a new PO-ARU message.  Otherwise,
   * we'll send it periodically in response to a timeout. */
  if(!SEND_PROOF_MATRIX_PERIODICALLY && !UTIL_I_Am_Leader())
    PRE_ORDER_Send_Proof_Matrix();

  /* The leader will send out Pre-Prepares periodically. */
}

void PRE_ORDER_Send_Proof_Matrix()
{
  signed_message *mset[NUM_SERVER_SLOTS];
  int32u num_parts, i, dest_bits;
  double time;

  /* Leader does not send proof matrix to itself */
  assert(!UTIL_I_Am_Leader());

  /* Make sure it's been long enough since we last sent a Proof Matrix */
  if(SEND_PROOF_MATRIX_PERIODICALLY) {
    UTIL_Stopwatch_Stop(&DATA.PO.proof_matrix_sw);
    time = UTIL_Stopwatch_Elapsed(&DATA.PO.proof_matrix_sw);
    if(time < (PO_PERIODICALLY_USEC / 1000000.0) * 
       (PROOF_MATRIX_PERIOD))
      return;
  }

  if (PRE_ORDER_Latest_Proof_Sent() || DATA.VIEW.view_change_done == 0) {
    /* already sent the latest proof, don't send it again */
    return;
  }

  PRE_ORDER_Construct_Proof_Matrix(mset, &num_parts);

  /* We are definitely sending the proof */
  PRE_ORDER_Update_Latest_Proof_Sent();

  /* Create DLL entry for measuring TAT of this proof matrix */
  PRE_ORDER_Create_TAT_Entry();

#if 0
  printf("    Challenging: Sending PM\n");
  int32u j;
  proof_matrix_message *pp = (proof_matrix_message *)(mset[1] + 1);
  po_aru_signed_message *cum_acks = (po_aru_signed_message *)(pp + 1);
  printf("++++++++++ SENDING MATRIX %u ++++++++++\n", DATA.ORD.seq - 1);
  for (i = 0; i < NUM_SERVERS; i++)
  {
    for (j = 0; j < NUM_SERVERS; j++)
    {
      printf("(%u, %u) ", cum_acks[i].cum_ack.ack_for_server[j].incarnation, cum_acks[i].cum_ack.ack_for_server[j].seq_num);
    }
    printf("\n");
  }
#endif

  for(i = 1; i <= num_parts; i++) {
    assert(mset[i]);

    /* Add the constructed part to the queue of messages to be signed.
     * The message will be sent only to the leader. */
    dest_bits = 0;
    UTIL_Bitmap_Set(&dest_bits, UTIL_Leader());
    SIG_Add_To_Pending_Messages(mset[i], dest_bits, 
				UTIL_Get_Timeliness(PROOF_MATRIX));
    dec_ref_cnt(mset[i]);
  }

  /* Mark that we've just sent a proof matrix so we don't do it again
   * for while. */
  UTIL_Stopwatch_Start(&DATA.PO.proof_matrix_sw);
}

bool PRE_ORDER_Latest_Proof_Sent() 
{
  int32u s;
  po_seq_pair ps;

  /* Has the most up to date proof already been sent?  Check to see if
   * the current proof contains new information that has not been sent
   * yet.  Returns FALSE if any slot is out of date. */
  for (s = 1; s <= NUM_SERVERS; s++) {
    ps = PRE_ORDER_Proof_ARU(s, DATA.PO.cum_acks+1);
    if(PRE_ORDER_Seq_Compare(ps, DATA.PO.max_num_sent_in_proof[s]) > 0) {
      return FALSE;
    }
  }

  return TRUE;
}

po_seq_pair PRE_ORDER_Proof_ARU(int32u server, po_aru_signed_message *proof) 
{
  int32u s;
  po_seq_pair cack[NUM_SERVER_SLOTS];

  /* A proof aru */

  for (s = 1; s <= NUM_SERVERS; s++)
    cack[s] = proof[s-1].cum_ack.ack_for_server[server-1];

  /*printf("  PO_Proof_ARU: [ ");
  for (s = 1; s <= NUM_SERVERS; s++)
    printf("(%u, %u) ", cack[s].incarnation, cack[s].seq_num);
  printf("]\n");*/

  /* sort the values */
  qsort( (void*)(cack+1), NUM_SERVERS, sizeof(po_seq_pair), poseqcmp);

  return cack[NUM_F + NUM_K + 1];
}

void PRE_ORDER_Update_Latest_Proof_Sent() 
{
  int32u s;
  po_seq_pair ps;
  
  /* We are sending a proof based on the current local po_arus */

  for (s = 1; s <= NUM_SERVERS; s++) {
    ps = PRE_ORDER_Proof_ARU(s, DATA.PO.cum_acks+1);
    if(PRE_ORDER_Seq_Compare(ps, DATA.PO.max_num_sent_in_proof[s]) > 0) {
      DATA.PO.max_num_sent_in_proof[s] = ps;
    }
  }
}

void PRE_ORDER_Create_TAT_Entry()
{
    tat_challenge tatc;
   
    /* Snapshot of PO ARU info */
    memcpy(&tatc.proof_matrix, &DATA.PO.cum_acks, 
                sizeof(po_aru_signed_message) * NUM_SERVER_SLOTS);

    /* Start measuring TAT - this is OK since aggregation delay due to signature
     * batching is already accounted for in the overall acceptable TAT */
    UTIL_Stopwatch_Start(&tatc.turnaround_time);
   
    /* Add this TAT challenge entry to the end of the list */
    stddll_push_back(&DATA.SUSP.turnaround_times, &tatc);
}

void PRE_ORDER_Process_Proof_Matrix(signed_message *mess)
{

  int32u s;
  po_aru_signed_message *cum_ack;
  proof_matrix_message *pm_specific;
  util_stopwatch sw;
  sp_time start;

  /* No need to apply my own Local Proof Matrix */
  if(VAR.My_Server_ID == mess->machine_id)
    return;

  Alarm(DEBUG, "Received a proof matrix from server %d\n", mess->machine_id);

  /* The proof is a collection of po_arus -- apply each one */
  pm_specific = (proof_matrix_message *)(mess + 1);
  cum_ack = (po_aru_signed_message *)(pm_specific + 1);
  
  /* timing tests */
  start.sec  = pm_specific->sec;
  start.usec = pm_specific->usec;
  sw.start = start;
  UTIL_Stopwatch_Stop(&sw);
  //if (UTIL_Stopwatch_Elapsed(&sw) > 0.004)
  //  Alarm(PRINT, "  PM from %2d, lat = %f ms\n", mess->machine_id, UTIL_Stopwatch_Elapsed(&sw) * 1000);

  for(s = 0; s < pm_specific->num_acks_in_this_message; s++)
    PRE_ORDER_Process_PO_ARU((signed_message *)&cum_ack[s]);
}

void PRE_ORDER_Garbage_Collect_PO_Slot(int32u server_id, po_seq_pair seq, int erase)
{
  po_slot *slot;
  recon_slot *r_slot;

  Alarm(DEBUG, "Garbage Collect PO Slot on [%d,%d,%d]\n", server_id, 
            seq.incarnation, seq.seq_num); 

  slot = UTIL_Get_PO_Slot_If_Exists(server_id, seq);

  /* Slot should not be NULL because in theory we just executed this
   * preordered request. */
  assert(slot != NULL);

  /* Clean out the PO-Request from the slot. With jumping, you may be cleaning up
   *   PO_slots without the po_request (po_request == NULL) */
  if (slot->po_request != NULL)
    dec_ref_cnt(slot->po_request);

  /* Now remove the slot itself */
  dec_ref_cnt(slot);
  if (erase) {
    stdhash_erase_key(&DATA.PO.History[server_id], &seq);
  }

  /* if(seq != (DATA.PO.white_line[server_id] + 1)) {
    Alarm(DEBUG, "Garbage collecting %d %d, white_line+1 = %d\n",
	  server_id, seq_num, DATA.PO.white_line[server_id]+1);
    //assert(0);
  } */
  DATA.PO.white_line[server_id] = seq;
  
  /* Sanity Check and FIX - Tom & Amy */
  if (PRE_ORDER_Seq_Compare(seq, DATA.PO.cum_aru[server_id]) > 0) {
    Alarm(DEBUG, "\tSHOULD NOT HAPPEN: White_line %d,%d surpassed cum_aru %d,%d "
                    "for server %d, fixing manually\n",
                    seq.incarnation, seq.seq_num, 
                    DATA.PO.cum_aru[server_id].incarnation,
                    DATA.PO.cum_aru[server_id].seq_num, 
                    server_id);
    DATA.PO.cum_aru[server_id] = seq;
  }

  /* If we had any reconciliation messages for this preorder id, discard
   * the associated slot. */
  if((r_slot = UTIL_Get_Recon_Slot_If_Exists(server_id, seq))) {
    assert(get_ref_cnt(r_slot) == 1);
    dec_ref_cnt(r_slot);
    stdhash_erase_key(&DATA.PO.Recon_History[server_id], &seq);
  }
}

/* Compares two po_seq_pair, returning:
    -1 if p1 < p2
     0 if p1 = p2
     1 if p1 > p2
*/
int PRE_ORDER_Seq_Compare(po_seq_pair p1, po_seq_pair p2)
{
    if (p1.incarnation < p2.incarnation)
        return -1;
    else if (p1.incarnation > p2.incarnation)
        return 1;
    else if (p1.seq_num < p2.seq_num)
        return -1;
    else if (p1.seq_num > p2.seq_num)
        return 1;
    return 0;
}

void PRE_ORDER_Initialize_Data_Structure()
{
  int32u s, s2;
  sp_time now;
  po_seq_pair zero_ps;

  //DATA.PO.debug_drop = 1;

  zero_ps.incarnation = 0;
  zero_ps.seq_num = 0;

  /* Grab a new incarnation, and setup our own sequence information */
  now = E_get_time();

  DATA.PO.po_seq.incarnation = now.sec;
  DATA.PO.po_seq.seq_num  = 0;

  DATA.PO.po_seq_executed.incarnation = now.sec;
  DATA.PO.po_seq_executed.seq_num = 0;

  DATA.PO.po_aru_num      = 0;

  /* Setup client sequence data */
  for (s = 1; s <= NUM_CLIENTS; s++) {
    DATA.PO.intro_client_seq[s]  = zero_ps;
    //DATA.PO.exec_client_seq[s]   = zero_ps;
  }
  DATA.PO.intro_client_seq[VAR.My_Server_ID].incarnation = now.sec;

  /* Setup ACK and ARU information */
  for (s = 1; s <= NUM_SERVERS; s++) {
    /* for each server, */
    DATA.PO.max_acked[s]             = zero_ps;
    DATA.PO.aru[s]                   = zero_ps;
    DATA.PO.cum_aru[s]               = zero_ps;
    DATA.PO.max_num_sent_in_proof[s] = zero_ps;
    DATA.PO.white_line[s]            = zero_ps;

    for(s2 = 1; s2 <= NUM_SERVERS; s2++)
      DATA.PO.cum_max_acked[s][s2] = zero_ps;
  }

  /* new progress flag */
  DATA.PO.cum_aru_updated = 0;

  memset(DATA.PO.cum_acks, (sizeof(po_aru_signed_message ) *
			    NUM_SERVER_SLOTS), 0);
  
  /* Construct the local PO History */
  for (s = 1; s <= NUM_SERVERS; s++) {
    stdhash_construct(&DATA.PO.History[s], sizeof(po_seq_pair),
		      sizeof(po_slot *), NULL, NULL, 0);
    stdhash_construct(&DATA.PO.Pending_Execution[s], sizeof(po_seq_pair),
		      sizeof(ord_slot *), NULL, NULL, 0);
    stdhash_construct(&DATA.PO.Recon_History[s], sizeof(po_seq_pair),
		      sizeof(recon_slot *), NULL, NULL, 0);
  }

  UTIL_Stopwatch_Start(&DATA.PO.po_request_sw);
  UTIL_Stopwatch_Start(&DATA.PO.po_ack_sw);
  UTIL_Stopwatch_Start(&DATA.PO.po_aru_sw);
  UTIL_Stopwatch_Start(&DATA.PO.proof_matrix_sw);
  UTIL_Stopwatch_Start(&DATA.PO.token_stopwatch);

  DATA.PO.tokens = 0;

  UTIL_DLL_Initialize(&DATA.PO.po_request_dll);
  UTIL_DLL_Initialize(&DATA.PO.proof_matrix_dll);

  /* Start trying to periodically send Pre-Order messages */
  PRE_ORDER_Periodically(0, NULL);

  PRE_ORDER_Periodic_Retrans(0, NULL);
}
