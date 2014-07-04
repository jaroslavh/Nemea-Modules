/**
 * \file loggerreplay.c
 * \brief Replay CSV file from logger (need -t that generates header).
 * \author Tomas Cejka <cejkat@cesnet.cz>
 * \date 2014
 */
/*
 * Copyright (C) 2014 CESNET
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
#include <inttypes.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <vector>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <libtrap/trap.h>
#include <unirec/unirec.h>


#define DYN_FIELD_MAX_SIZE 512 // Maximum size of dynamic field, longer fields will be cutted to this size

ur_field_id_t ur_get_id_by_name(const char *name);

// Struct with information about module
trap_module_info_t module_info = {
   (char *) "Loggerreplay", // Module name
   // Module description
   (char *) "This module converts CSV from logger and sends it in UniRec.\n"
   "CSV is expected to have UniRec specifier in the first line (logger -t).\n"
   "\n"
   "Interfaces:\n"
   "   Inputs: 0\n"
   "   Outputs: 1\n"
   "\n"
   "Usage:\n"
   "   ./logger -i IFC_SPEC -f FILE\n"
   "\n"
   "Module specific parameters:\n"
   "   UNIREC_FMT   The i-th parameter of this type specifies format of UniRec\n"
   "                expected on the i-th input interface.\n"
   "   -f FILE      Read FILE.\n"
   "                first field (or second when -n is specified).\n"
   "   -c N         Quit after N records are received.\n"
//   "   -s C         Field separator (default ',').\n"
//   "   -r C         Record separator (default '\\n').\n"
   ,
   0, // Number of input interfaces (-1 means variable)
   1, // Number of output interfaces
};

static int stop = 0;

int verbose;
static int n_inputs; // Number of input interfaces
static ur_template_t **templates; // UniRec templates of input interfaces (array of length n_inputs)
static ur_template_t *out_template; // UniRec template with union of fields of all inputs
int print_ifc_num = 0;
int print_time = 0;

unsigned int num_records = 0; // Number of records received (total of all inputs)
unsigned int max_num_records = 0; // Exit after this number of records is received


static FILE *file; // Output file

TRAP_DEFAULT_SIGNAL_HANDLER(stop = 1);

using namespace std;

void store_value(ur_template_t *t, void *data, int f_id, string &column)
{
   // Check size of dynamic field and if longer than maximum size then cut it
   if (column.length() > DYN_FIELD_MAX_SIZE) {
      column[DYN_FIELD_MAX_SIZE] = 0;
   }
   ur_set_from_string(t, data, f_id, column.c_str());
}

ur_field_id_t urgetidbyname(const char *name)
{
   for (int id = 0; id < UR_FIELDS_NUM; id++) {
      if (strcmp(name, UR_FIELD_NAMES[id]) == 0) {
         return id;
      }
   }
   return UR_INVALID_FIELD;
}

string replace_string(string subject, const string &search, const string &replace) {
   size_t pos = 0;
   while ((pos = subject.find(search, pos)) != std::string::npos) {
      subject.replace(pos, search.length(), replace);
      pos += replace.length();
   }
   return subject;
}

int main(int argc, char **argv)
{
   int ret;
   char *out_template_str = NULL;
   char *in = NULL, *in_filename = NULL;
   char record_delim = '\n';
   char field_delim = ',';
   ifstream f_in;
   string line;
   ur_template_t *utmpl = NULL;
   void *data = NULL;
   // ***** Process parameters *****

   // Let TRAP library parse command-line arguments and extract its parameters
   trap_ifc_spec_t ifc_spec;
   ret = trap_parse_params(&argc, argv, &ifc_spec);
   if (ret != TRAP_E_OK) {
      if (ret == TRAP_E_HELP) { // "-h" was found
         trap_print_help(&module_info);
         return 0;
      }
      fprintf(stderr, "ERROR in parsing of parameters for TRAP: %s\n", trap_last_error_msg);
      return 1;
   }

   verbose = trap_get_verbose_level();
   if (verbose >= 0){
      printf("Verbosity level: %i\n", trap_get_verbose_level());
   }

   // Parse remaining parameters and get configuration
   char opt;
   while ((opt = getopt(argc, argv, "f:c:" /* r:s: */)) != -1) {
      switch (opt) {
         case 'f':
            in_filename = optarg;
            break;
         case 'c':
            max_num_records = atoi(optarg);
            if (max_num_records == 0) {
               fprintf(stderr, "Error: Parameter of -c option must be integer > 0.\n");
               return 1;
            }
            break;
         //case 's':
         //   field_delim = (optarg[0] != '\\' ? optarg[0] : (optarg[1] == 't'?'\t':'\n'));
         //   printf("Field delimiter: 0x%02X\n", field_delim);
         //   break;
         //case 'r':
         //   record_delim = (optarg[0] != '\\' ? optarg[0] : (optarg[1] == 't'?'\t':'\n'));
         //   printf("Record delimiter: 0x%02X\n", record_delim);
         //   break;
         default:
            fprintf(stderr, "Error: Invalid arguments.\n");
            return 1;
      }
   }

   // Create UniRec templates
   n_inputs = argc - optind;
   if (verbose >= 0) {
      printf("Number of inputs: %i\n", n_inputs);
   }

   f_in.open(in_filename);
   trap_ctx_t *ctx = NULL;

   if (f_in.good()) {
      getline(f_in, line, record_delim);
      utmpl = ur_create_template(line.c_str());
      if (utmpl == NULL) {
         goto exit;
      }

     // calculate maximum needed memory for dynamic fields
     int memory_needed = 0;
     ur_field_id_t field_id = UR_INVALID_FIELD;
     while ((field_id = ur_iter_fields(utmpl, field_id)) != UR_INVALID_FIELD) {
        if (ur_is_dynamic(field_id) != 0) {
           memory_needed += DYN_FIELD_MAX_SIZE;
        }
     }

      data = ur_create(utmpl, memory_needed);
      if (data == NULL) {
         goto exit;
      }

      // Initialize TRAP library (create and init all interfaces)
      if (verbose >= 0) {
         printf("Initializing TRAP library ...\n");
      }
      ctx = trap_ctx_init(&module_info, ifc_spec);
      if (ret != TRAP_E_OK) {
         fprintf(stderr, "ERROR in TRAP initialization: %s\n", trap_last_error_msg);
         trap_free_ifc_spec(ifc_spec);
         ret = 2;
         goto exit;
      }

      // We don't need ifc_spec anymore, destroy it
      trap_free_ifc_spec(ifc_spec);
      trap_ctx_ifcctl(ctx, TRAPIFC_OUTPUT, 0, TRAPCTL_SETTIMEOUT, TRAP_WAIT);
      trap_ctx_ifcctl(ctx, TRAPIFC_OUTPUT, 0, TRAPCTL_BUFFERSWITCH, 0);

      stringstream ss(line);
      vector<ur_field_id_t> field_ids;
      string column;
      // get field ids from template
      ur_field_id_t tmpl_f_id = UR_INVALID_FIELD;
      while ((tmpl_f_id = ur_iter_fields_tmplt(utmpl, tmpl_f_id)) != UR_INVALID_FIELD) {
         field_ids.push_back(tmpl_f_id);
      }

      /* main loop */
      while (f_in.good()) {
         getline(f_in, line, record_delim);
         if (!f_in.good()) {
            break;
         }
         stringstream sl(line);
         for (vector<ur_field_id_t>::iterator it = field_ids.begin(); it != field_ids.end(); ++it) {
            // check if current field is dynamic
            char cur_field_delim;
            if (ur_is_dynamic(*it) != 0) {
               // dynamic fields delimeter
               // (dynamic fields could contain ',' so it can't be delimeter for them)
               cur_field_delim = '"';
               getline(sl, column, cur_field_delim); // trim first '"' 
            } else {
               // static fields delimeter
               cur_field_delim = field_delim;
            }

            getline(sl, column, cur_field_delim);
            store_value(utmpl, data, *it, column);
         }
         trap_ctx_send(ctx, 0, data, ur_rec_size(utmpl, data));
      }
   }

   // ***** Cleanup *****

exit:
   if (f_in.is_open()) {
      f_in.close();
   }
   if (verbose >= 0) {
      printf("Exitting ...\n");
   }
   
   trap_ctx_send_flush(ctx, 0);
   trap_ctx_finalize(&ctx);

   if (utmpl != NULL) {
      ur_free_template(utmpl);
      utmpl = NULL;
   }
   if (data != NULL) {
      ur_free(data);
      data = NULL;
   }

   return ret;
}
