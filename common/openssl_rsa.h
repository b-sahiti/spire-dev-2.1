/*
 * Spire.
 *
 * The contents of this file are subject to the Spire Open-Source
 * License, Version 1.0 (the ``License''); you may not use
 * this file except in compliance with the License.  You may obtain a
 * copy of the License at:
 *
 * http://www.dsn.jhu.edu/spire/LICENSE.txt 
 *
 * or in the file ``LICENSE.txt'' found in this distribution.
 *
 * Software distributed under the License is distributed on an AS IS basis, 
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License 
 * for the specific language governing rights and limitations under the 
 * License.
 *
 * Spire is developed at the Distributed Systems and Networks Lab,
 * Johns Hopkins University and the Resilient Systems and Societies Lab,
 * University of Pittsburgh.
 *
 * Creators:
 *   Yair Amir            yairamir@cs.jhu.edu
 *   Trevor Aron          taron1@cs.jhu.edu
 *   Amy Babay            babay@pitt.edu
 *   Thomas Tantillo      tantillo@cs.jhu.edu 
 *   Sahiti Bommareddy    sahiti@cs.jhu.edu 
 *   Maher Khan           maherkhan@pitt.edu
 *
 * Major Contributors:
 *   Marco Platania       Contributions to architecture design 
 *   Daniel Qian          Contributions to Trip Master and IDS 
 *
 * Contributors:
 *   Samuel Beckley       Contributions to HMIs
 *
 * Copyright (c) 2017-2024 Johns Hopkins University.
 * All rights reserved.
 *
 * Partial funding for Spire research was provided by the Defense Advanced 
 * Research Projects Agency (DARPA), the Department of Defense (DoD), and the
 * Department of Energy (DoE).
 * Spire is not necessarily endorsed by DARPA, the DoD or the DoE. 
 *
 */

/* Openssl initiation, sign, and verify functions. These functions can be used
 * to easily use openssl to create RSA signatures and to verify these RSA
 * signatures. The functions listed below use RSA and sha1 digests. */

#ifndef PRIME_OPENSSL_RSA_H
#define PRIME_OPENSSL_RSA_H

#include "net_wrapper.h"
#include <stdio.h>
#include <openssl/rsa.h>
//#include "arch.h"

/* Public definitions */
#define RSA_CLIENT         1
#define RSA_SERVER         2
#define RSA_CONFIG_MNGR    3
#define RSA_CONFIG_AGENT   4

/* Public functions */
void OPENSSL_RSA_Init();

void OPENSSL_RSA_Sign( const unsigned char *message, size_t message_length,
		       unsigned char *signature ); 

int OPENSSL_RSA_Verify( const unsigned char *message, size_t message_length,
			unsigned char *signature, int32u server_number, 
			int32u type ); 
 
void OPENSSL_RSA_Read_Keys( int32u my_number, int32u type, const char *keys_dir ); 

void OPENSSL_RSA_Reload_Prime_Keys( int32u my_number, int32u type, const char *keys_dir,int32u curr_servers ); 

void OPENSSL_RSA_Generate_Keys( const char *keys_dir ); 

void OPENSSL_RSA_Generate_Keys_with_args(int count, const char *keys_dir ); 

void OPENSSL_RSA_Make_Signature( const unsigned char *digest_value, 
				 unsigned char *signature ); 

int32u OPENSSL_RSA_Verify_Signature( const unsigned char *digest_value, 
				     unsigned char *signature, int32u number, 
				     int32u type ); 

int32u OPENSSL_RSA_Digests_Equal( unsigned char *digest1, 
				  unsigned char *digest2 ); 

void OPENSSL_RSA_Make_Digest( const void *buffer, size_t buffer_size, 
			      unsigned 	char *digest_value ); 

void OPENSSL_RSA_Print_Digest( unsigned char *digest_value ); 


int OPENSSL_RSA_Get_KeySize(unsigned char *pubKeyFile);


void OPENSSL_RSA_Decrypt(unsigned char  *pvtKeyFile,unsigned char *data, int data_len,unsigned char *decrypted_data);


int OPENSSL_RSA_Encrypt(unsigned char *pubKeyFile,unsigned char *data, int data_len, unsigned char * encrypted_data);

int getFileSize(unsigned char * fileName);
#endif
