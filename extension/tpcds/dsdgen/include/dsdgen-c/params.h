/*
 * Legal Notice
 *
 * This document and associated source code (the "Work") is a part of a
 * benchmark specification maintained by the TPC.
 *
 * The TPC reserves all right, title, and interest to the Work as provided
 * under U.S. and international laws, including without limitation all patent
 * and trademark rights therein.
 *
 * No Warranty
 *
 * 1.1 TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, THE INFORMATION
 *     CONTAINED HEREIN IS PROVIDED "AS IS" AND WITH ALL FAULTS, AND THE
 *     AUTHORS AND DEVELOPERS OF THE WORK HEREBY DISCLAIM ALL OTHER
 *     WARRANTIES AND CONDITIONS, EITHER EXPRESS, IMPLIED OR STATUTORY,
 *     INCLUDING, BUT NOT LIMITED TO, ANY (IF ANY) IMPLIED WARRANTIES,
 *     DUTIES OR CONDITIONS OF MERCHANTABILITY, OF FITNESS FOR A PARTICULAR
 *     PURPOSE, OF ACCURACY OR COMPLETENESS OF RESPONSES, OF RESULTS, OF
 *     WORKMANLIKE EFFORT, OF LACK OF VIRUSES, AND OF LACK OF NEGLIGENCE.
 *     ALSO, THERE IS NO WARRANTY OR CONDITION OF TITLE, QUIET ENJOYMENT,
 *     QUIET POSSESSION, CORRESPONDENCE TO DESCRIPTION OR NON-INFRINGEMENT
 *     WITH REGARD TO THE WORK.
 * 1.2 IN NO EVENT WILL ANY AUTHOR OR DEVELOPER OF THE WORK BE LIABLE TO
 *     ANY OTHER PARTY FOR ANY DAMAGES, INCLUDING BUT NOT LIMITED TO THE
 *     COST OF PROCURING SUBSTITUTE GOODS OR SERVICES, LOST PROFITS, LOSS
 *     OF USE, LOSS OF DATA, OR ANY INCIDENTAL, CONSEQUENTIAL, DIRECT,
 *     INDIRECT, OR SPECIAL DAMAGES WHETHER UNDER CONTRACT, TORT, WARRANTY,
 *     OR OTHERWISE, ARISING IN ANY WAY OUT OF THIS OR ANY OTHER AGREEMENT
 *     RELATING TO THE WORK, WHETHER OR NOT SUCH AUTHOR OR DEVELOPER HAD
 *     ADVANCE NOTICE OF THE POSSIBILITY OF SUCH DAMAGES.
 *
 * Contributors:
 * Gradient Systems
 */
#ifndef QGEN_PARAMS_H
#define QGEN_PARAMS_H

#include "r_params.h"
#include "release.h"
#include "build_support.h"

#ifdef DECLARER

thread_local option_t options[] = {{"ABREVIATION", OPT_STR, 0, "build table with abreviation <s>", NULL, ""},
                      {"DELIMITER", OPT_STR | OPT_ADV, 1, "use <s> as output field separator", NULL, "|"},
                      {"DIR", OPT_STR, 2, "generate tables in directory <s>", NULL, "."},
                      {"DISTRIBUTIONS", OPT_STR | OPT_ADV, 3, "read distributions from file <s>", NULL, "NONE"},
                      {"FORCE", OPT_FLG | OPT_ADV, 4, "over-write data files without prompting", NULL, "N"},
                      {"HELP", OPT_INT, 5, "display this message", NULL, "0"},
                      {"PARAMS", OPT_STR, 6, "read parameters from file <s>", NULL, ""},
                      {"PROG", OPT_STR | OPT_HIDE | OPT_SET, 7, "DO NOT MODIFY", NULL, "dsdgen"},
                      {"QUIET", OPT_FLG, 8, "disable all output to stdout/stderr", NULL, "N"},
                      {"SCALE", OPT_STR, 9, "volume of data to generate in GB", SetScaleIndex, "1"},
                      {"SUFFIX", OPT_STR | OPT_ADV, 10, "use <s> as output file suffix", NULL, ".dat"},
                      {"TABLE", OPT_STR, 11, "build only table <s>", NULL, "ALL"},
                      {"TERMINATE", OPT_FLG | OPT_ADV, 12, "end each record with a field delimiter", NULL, "Y"},
                      {"UPDATE", OPT_INT, 13, "generate update data set <n>", NULL, ""},
                      {"VERBOSE", OPT_FLG, 14, "enable verbose output", NULL, "N"},
                      {"_SCALE_INDEX", OPT_INT | OPT_HIDE, 15, "Scale band; used for dist lookups", NULL, "1"},
                      {"PARALLEL", OPT_INT, 16, "build data in <n> separate chunks", NULL, ""},
                      {"CHILD", OPT_INT, 17, "generate <n>th chunk of the parallelized data", NULL, "1"},
                      {"CHKSEEDS", OPT_FLG | OPT_HIDE, 18, "validate RNG usage for parallelism", NULL, "N"},
                      {"RELEASE", OPT_FLG, 19, "display the release information", NULL, "N"},
                      {"_FILTER", OPT_FLG, 20, "output data to stdout", NULL, "N"},
                      {"VALIDATE", OPT_FLG, 21, "produce rows for data validation", NULL, "N"},
                      {"VCOUNT", OPT_INT | OPT_ADV, 22, "set number of validation rows to be produced", NULL, "50"},
                      {"VSUFFIX", OPT_STR | OPT_ADV, 23, "set file suffix for data validation", NULL, ".vld"},
                      {"RNGSEED", OPT_INT | OPT_ADV, 24, "set RNG seed", NULL, "19620718"},
                      {NULL}};

thread_local char *params[23 + 2];
#else
extern thread_local option_t options[];
extern thread_local char *params[];
extern char *szTableNames[];
#endif

#endif
