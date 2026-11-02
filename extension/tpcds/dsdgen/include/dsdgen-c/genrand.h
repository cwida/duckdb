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
#ifndef GENRAND_H
#define GENRAND_H

#include "decimal.h"
#include "date.h"
#include "dist.h"
#include "address.h"
#define JMS 1

typedef struct RNG_T {
	int nUsed;
	int nUsedPerRow;
	long nSeed;
	long nInitialSeed; /* used to allow skip_row() to back up */
	int nColumn;       /* column where this stream is used */
	int nTable;        /* table where this stream is used */
	int nDuplicateOf;  /* duplicate streams allow independent tables to share
	                      data streams */
#ifdef JMS
	ds_key_t nTotal;
#endif
} rng_t;
extern thread_local rng_t Streams[];

#define FL_SEED_OVERRUN 0x0001

#define ALPHANUM "abcdefghijklmnopqrstuvxyzABCDEFGHIJKLMNOPQRSTUVXYZ0123456789"
#define DIGITS   "0123456789"

#define RNG_SEED 19620718

int genrand_integer(int *dest, int dist, int min, int max, int mean, int stream);
int genrand_decimal(decimal_t *dest, int dist, decimal_t *min, decimal_t *max, decimal_t *mean, int stream);
int genrand_date(date_t *dest, int dist, date_t *min, date_t *max, date_t *mean, int stream);
ds_key_t genrand_key(ds_key_t *dest, int dist, ds_key_t min, ds_key_t max, ds_key_t mean, int stream);
int gen_charset(char *dest, char *set, int min, int max, int stream);
void init_rand(void);
void skip_random(int s, ds_key_t count);
int RNGReset(int nTable);
long next_random(int nStream);
void genrand_email(char *pEmail, char *pFirst, char *pLast, int nColumn);
void genrand_ipaddr(char *pDest, int nColumn);
int genrand_url(char *pDest, int nColumn);
int setSeed(int nStream, int nValue);
void resetSeeds(int nTable);

#endif
