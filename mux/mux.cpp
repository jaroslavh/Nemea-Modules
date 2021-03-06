/**
 * \file mux.cpp
 * \brief Unite traffic incoming on multiple interface into one output stream (input for demux module).
 * \author Dominik Soukup <soukudom@fit.cvut.cz>
 * \date 1/2018
 */
/*
 * Copyright (C) 2018 CESNET
 *
 * LICENSE TERMS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <iostream>
#include <unirec/unirec.h>
#include <libtrap/trap.h>
#include <getopt.h>
#include <omp.h>
#include <signal.h>

using namespace std;
int exit_value=0;
static ur_template_t **in_templates = NULL; //UniRec input templates
static int n_inputs = 0; //number of input interface
trap_ctx_t *ctx = NULL;
int stop = 0;
int ret = 2;
int verbose = 0;
trap_module_info_t *module_info = NULL;

typedef struct meta_info_s {
   uint16_t messageID; //1 - normal data, 2 - hello message (unirec fmt changed)
   uint16_t interfaceID;
   uint8_t data_fmt;
   char payload[0];
} meta_info_t;

#define MODULE_BASIC_INFO(BASIC) \
  BASIC("mux", "This module unites more input interfaces into one output interface", -1, 1)
#define MODULE_PARAMS(PARAM) \
PARAM('n', "link_count", "Sets count of input links. Must correspond to parameter -i (trap).", required_argument, "int32")

void capture_thread(int index)
{

   const void *data_nemea_input = NULL;
   uint16_t memory_received = 0;
   uint8_t data_fmt = TRAP_FMT_UNKNOWN;
   const char *spec = NULL;

   //set output interface format
   trap_ctx_set_data_fmt(ctx, 0, TRAP_FMT_RAW);

   //allocate structure with new header and data payload
   meta_info_t *meta_data;
   char *buffer[65535];
   meta_data = (meta_info_t *) buffer;
   //main loop
   while (!stop) {
      ret = trap_ctx_recv(ctx, index, &data_nemea_input, &memory_received);
      //=== process received data ===
      if (ret == TRAP_E_OK || ret == TRAP_E_FORMAT_CHANGED) {
         if (ret == TRAP_E_FORMAT_CHANGED) {
            //get input interface format
            if (trap_ctx_get_data_fmt(ctx, TRAPIFC_INPUT, index, &data_fmt, &spec) != TRAP_E_OK) {
               cerr << "ERROR: Data format was not loaded." << endl;
               return;
            }

            if (verbose >= 0) {
               cout << "Data format has been changed. Sending hello message" << endl;
            }

            //fill in hello message
            meta_data->messageID = 2;
            meta_data->interfaceID = index;
            meta_data->data_fmt = data_fmt;
            memcpy(meta_data->payload, spec, strlen(spec) + 1);
#pragma omp critical
            {
               ret = trap_ctx_send(ctx, 0, buffer, sizeof(*meta_data) + strlen(spec) + 1);
            }

         }

         //forward received payload data
         meta_data->messageID = 1;
         meta_data->interfaceID = index;
         meta_data->data_fmt = data_fmt;
         memcpy(meta_data->payload, data_nemea_input, memory_received);
      } else{
         //endif ret == TRAP_E_OK | ret == TRAP_E_FORMAT_CHANGED
         cerr << "ERROR: Undefined option on input interface" << endl;
         meta_data->messageID = -1;
         meta_data->interfaceID = index;
      }

      //send data out
#pragma omp critical
      {
         ret = trap_ctx_send(ctx, 0, buffer, sizeof(*meta_data) + memory_received);
         if (verbose >= 0) {
            cout << "Iterface with index " << index << " sent data out" << endl;
         }
      }
   } //end while (!stop)
}

int main (int argc, char ** argv)
{
   //allocate and initialize module_info structure and all its members
   INIT_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
   //trap parameters processing
   trap_ifc_spec_t ifc_spec;
   ret = trap_parse_params(&argc, argv, &ifc_spec);
   if (ret != TRAP_E_OK) {
      if (ret == TRAP_E_HELP) { // "-h" was found
         trap_print_help(module_info);
         return 0;
      }
      cerr << "ERROR in parsing of parameters for TRAP: " << trap_last_error_msg << endl;
      return 1;
   }

   //parse remaining parameters and get configuration
   signed char opt;
   while ((opt = TRAP_GETOPT(argc, argv, module_getopt_string, long_options)) != -1) {
      switch (opt) {
      case 'n':
         n_inputs = atoi(optarg);
         break;
      default:
         cerr << "Error: Invalid arguments." << endl;
         FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
         return 1;
      }
   }

   verbose = trap_get_verbose_level();
   if (verbose >= 0) {
      cout << "Verbosity level: " <<  trap_get_verbose_level() << endl;;
   }

   if (verbose >= 0) {
      cerr << "Number of inputs: " <<  n_inputs << endl;
   }

   //check input parameter
   if (n_inputs < 1) {
      cerr <<  "Error: Number of input interfaces must be positive integer." << endl;
      exit_value = 2;
      goto cleanup;
   }
   if (n_inputs > 32) {
      cerr << "Error: More than 32 interfaces is not allowed by TRAP library." << endl;
      exit_value = 2;
      goto cleanup;
   }

   // Set number of input interfaces
   module_info->num_ifc_in = n_inputs;

   if (verbose >= 0) {
      cout << "Initializing TRAP library ..." << endl;
   }

   ctx = trap_ctx_init(module_info, ifc_spec);

   if (ctx == NULL) {
      cerr << "ERROR in TRAP initialization: " << trap_last_error_msg << endl;
      exit_value = 3;
      goto cleanup;
   }
    
   if (trap_ctx_get_last_error(ctx) != TRAP_E_OK){
      cerr << "ERROR in TRAP initialization: " << trap_ctx_get_last_error_msg(ctx) << endl;
      exit_value = 3;
      goto cleanup;
   } 


   //check if number of interfaces is correct
   if (strlen(ifc_spec.types) <= 1) {
      cerr << "ERROR expected at least 1 input and 1 output interface. Got only 1." << endl;
      return 2;
   }

   //output interface control settings
   if (trap_ctx_ifcctl(ctx, TRAPIFC_OUTPUT, 0, TRAPCTL_SETTIMEOUT, TRAP_NO_WAIT) != TRAP_E_OK) {
      cerr << "ERROR in output interface initialization" << endl;
      exit_value = 3;
      goto cleanup;
   }


   //allocate memory for input templates
   in_templates = (ur_template_t**) calloc(n_inputs, sizeof(*in_templates));
   if (in_templates == NULL) {
      cerr <<  "Memory allocation error." << endl;
      exit_value = 3;
      goto cleanup;
   }

   //initialize and set templates for UniRec negotiation
   for (int i = 0; i < n_inputs; i++) {
      //input interfaces control settings
      if (trap_ctx_ifcctl(ctx, TRAPIFC_INPUT, i, TRAPCTL_SETTIMEOUT, TRAP_WAIT) != TRAP_E_OK) {
         cerr << "ERROR in input interface initialization" << endl;
         exit_value = 3;
         goto cleanup;
      }
      //create empty input template
      in_templates[i] = ur_ctx_create_input_template(ctx, i, NULL, NULL);
      if (in_templates[i] == NULL) {
         cerr <<  "Memory allocation error." << endl;
         exit_value = 3;
         goto cleanup;
      }
      //set required incoming format
      trap_ctx_set_required_fmt(ctx, i, TRAP_FMT_UNIREC, NULL);
   }

   if (verbose >= 0) {
      cout << "Initialization done" << endl;
   }

#pragma omp parallel num_threads(n_inputs)
   {
      capture_thread(omp_get_thread_num());
   }

cleanup:
   //cleaning
   FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)
   trap_ctx_finalize(&ctx);
   
   return exit_value;
}

