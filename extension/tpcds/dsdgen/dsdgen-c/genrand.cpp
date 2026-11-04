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
#include "config.h"
#include "init.h"
#include "porting.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <search.h>
#include <limits.h>
#endif
#include "config.h"
#include "porting.h"
#include "decimal.h"
#include "date.h"
#include "genrand.h"
#include "dist.h"
#include "r_params.h"
#include "params.h"

#include "columns.h"
#include "tables.h"
#include "streams.h"

#include <cstring>

static long Mult = 16807; /* the multiplier */
static long nQ = 127773;  /* the quotient MAXINT / Mult */
static long nR = 2836;    /* the remainder MAXINT % Mult */
void DSNthElement(HUGE_TYPE N, int nStream);

/*
 * Routine: next_random(int stream)
 * Purpose:
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
long next_random(int stream) {
	long s = Streams[stream].nSeed, div_res, mod_res;

	div_res = s / nQ;
	mod_res = s - nQ * div_res; /* i.e., mod_res = s % nQ */
	s = Mult * mod_res - div_res * nR;
	if (s < 0)
		s += MAXINT;
	Streams[stream].nSeed = s;
	Streams[stream].nUsed += 1;
#ifdef JMS
	Streams[stream].nTotal += 1;
#endif
	return (s);
}

/*
 * Routine: next_random_float(int stream)
 * Purpose:  return random in [0..1]
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
double next_random_float(int stream) {
	long res;

	res = next_random(stream);

	return ((double)res / (double)MAXINT);
}

/*
 * Routine: skip_random(int stream, int skip_count)
 * Purpose:
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
void skip_random(int nStream, ds_key_t N) {
	ds_key_t Z;
	ds_key_t M;

#ifdef UNDEF
	fprintf(stderr, "skipping stream %d to %d\n", nStream, N);
	Streams[nStream].nTotal = N;
#endif
	M = Mult;
	Z = (ds_key_t)Streams[nStream].nInitialSeed;
	while (N > 0) {
		if (N % 2 != 0) /* testing for oddness, this seems portable */
			Z = (M * Z) % MAXINT;
		N = N / 2; /* integer division, truncates */
		M = (M * M) % MAXINT;
	}
	Streams[nStream].nSeed = (long)Z;

	return;
}

/*
 * Routine: genrand_integer(int dist, int min, int max, int mean)
 * Purpose: generate a random integer given the distribution and limits
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns: int
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 */
int genrand_integer(int *dest, int dist, int min, int max, int mean, int stream) {
	int res = 0, i;
	double fres = 0;

	switch (dist) {
	case DIST_UNIFORM:
		res = next_random(stream);
		res %= max - min + 1;
		res += min;
		break;
	case DIST_EXPONENTIAL:
		for (i = 0; i < 12; i++)
			fres += (double)(next_random(stream) / MAXINT) - 0.5;
		res = min + (int)((max - min + 1) * fres);
		break;
	default:
		INTERNAL("Undefined distribution");
		break;
	}

	if (dest == NULL)
		return (res);

	*dest = res;

	return (0);
}

/*
 * Routine: genrand_key(ket_t *dest, int dist, ds_key_t min, ds_key_t max,
 * ds_key_t mean, int stream) Purpose: generate a random integer given the
 * distribution and limits Algorithm: Data Structures:
 *
 * Params:
 * Returns: ds_key_t
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: Need to rework to rely on RNG routines that will work for 64 bit return
 * values
 */
ds_key_t genrand_key(ds_key_t *dest, int dist, ds_key_t min, ds_key_t max, ds_key_t mean, int stream) {
	int res = 0, i;
	double fres = 0;

	switch (dist) {
	case DIST_UNIFORM:
		res = next_random(stream);
		res %= (int)(max - min + 1);
		res += (int)min;
		break;
	case DIST_EXPONENTIAL:
		for (i = 0; i < 12; i++)
			fres += (double)(next_random(stream) / MAXINT) - 0.5;
		res = (int)min + (int)((max - min + 1) * fres);
		break;
	default:
		INTERNAL("Undefined distribution");
		break;
	}

	if (dest == NULL)
		return ((ds_key_t)res);

	*dest = (ds_key_t)res;

	return ((ds_key_t)0);
}

/*
 * Routine:
 *	genrand_decimal(int dist, decimal_t *min, decimal_t *max, decimal_t *mean)
 * Purpose: create a random decimal_t
 * Algorithm:
 * Data Structures:
 *
 * Params: min/max are char * to allow easy passing of precision
 * Returns: decimal_t *; NULL on failure
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int genrand_decimal(decimal_t *dest, int dist, decimal_t *min, decimal_t *max, decimal_t *mean, int stream) {
	int i;
	decimal_t res;
	double fres = 0.0;

	if (min->precision < max->precision)
		dest->precision = min->precision;
	else
		dest->precision = max->precision;

	switch (dist) {
	case DIST_UNIFORM:
		res.number = next_random(stream);
		res.number %= max->number - min->number + 1;
		res.number += min->number;
		break;
	case DIST_EXPONENTIAL:
		for (i = 0; i < 12; i++) {
			fres /= 2.0;
			fres += (double)((double)next_random(stream) / (double)MAXINT) - 0.5;
		}
		res.number = mean->number + (int)((max->number - min->number + 1) * fres);
		break;
	default:
		INTERNAL("Undefined distribution");
		break;
	}

	dest->number = res.number;
	i = 0;
	while (res.number > 10) {
		res.number /= 10;
		i += 1;
	}
	dest->scale = i;

	return (0);
}

/* Routine: RNGReset(int tbl)
 * Purpose:
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int RNGReset(int tbl) {
	int i;

	for (i = 0; Streams[i].nColumn != -1; i++)
		if (Streams[i].nTable == tbl)
			Streams[i].nSeed = Streams[i].nInitialSeed;

	return (0);
}

/* WARNING!  This routine assumes the existence of 64-bit                 */

/* integers.  The notation used here- "HUGE" is *not* ANSI standard. */

/* Hopefully, you have this extension as well.  If not, use whatever      */

/* nonstandard trick you need to in order to get 64 bit integers.         */

/* The book says that this will work if MAXINT for the type you choose    */

/* is at least 2**46  - 1, so 64 bits is more than you *really* need      */

static HUGE_TYPE Multiplier = 16807;   /* or whatever nonstandard */
static HUGE_TYPE Modulus = 2147483647; /* trick you use to get 64 bit int */

/* Advances value of Seed after N applications of the random number generator
   with multiplier Mult and given Modulus.
   NthElement(Seed[],count);

   Theory:  We are using a generator of the form
        X_n = [Mult * X_(n-1)]  mod Modulus.    It turns out that
        X_n = [(Mult ** n) X_0] mod Modulus.
   This can be computed using a divide-and-conquer technique, see
   the code below.

   In words, this means that if you want the value of the Seed after n
   applications of the generator,  you multiply the initial value of the
   Seed by the "super multiplier" which is the basic multiplier raised
   to the nth power, and then take mod Modulus.
*/

/* Nth Element of sequence starting with StartSeed */
void DSNthElementNthElement(HUGE_TYPE N, int nStream) {
	HUGE_TYPE Z;
	HUGE_TYPE Mult;

	Mult = Multiplier;
	Z = (HUGE_TYPE)Streams[nStream].nInitialSeed;
	while (N > 0) {
		if (N % 2 != 0) /* testing for oddness, this seems portable */
		{
#ifdef JMS
			Streams[nStream].nTotal += 1;
#endif
			Z = (Mult * Z) % Modulus;
		}
		N = N / 2; /* integer division, truncates */
		Mult = (Mult * Mult) % Modulus;
#ifdef JMS
		Streams[nStream].nTotal += 2;
#endif
	}
	Streams[nStream].nSeed = (long)Z;

	return;
}

/*
 * Routine: gen_charset(char *set, int min, int max)
 * Purpose: generate random characters from set for a random length [min..max]
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int gen_charset(char *dest, char *set, int min, int max, int stream) {
	int len, i, temp;

	if (set == NULL) {
		dest = NULL;
		return (-1);
	}

	genrand_integer(&len, DIST_UNIFORM, min, max, 0, stream);

	for (i = 0; i < max; i++) {
		genrand_integer(&temp, DIST_UNIFORM, 0, strlen(set) - 1, 0, stream);
		if (i < len)
			dest[i] = *(set + temp);
	}
	dest[len] = '\0';

	return (0);
}

/*
 * Routine: genrand_date(int dist, date_t *min, date_t *max)
 * Purpose: generate random date within [min..max]
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int genrand_date(date_t *dest, int dist, date_t *min, date_t *max, date_t *mean, int stream) {
	int range, imean = 0, temp, idt, nYear, nTotalWeight = 0, nDayCount;

	idt = dttoj(min);
	range = dttoj(max);
	range -= idt;
	nDayCount = min->day;
	nYear = min->year;

	switch (dist) {
	case DIST_SALES:
	case DIST_RETURNS:
		/* walk from min to max to "integrate" the distribution */
		while (range -= 1) {
			nTotalWeight += dist_weight(NULL, "calendar", nDayCount, dist + is_leap(nYear));
			if (nDayCount == 365 + is_leap(nYear)) {
				nYear += 1;
				nDayCount = 1;
			} else
				nDayCount += 1;
		}
		/* pick a value in the resulting range */
		temp = genrand_integer(NULL, DIST_UNIFORM, 1, nTotalWeight, 0, stream);
		/* and walk it again to translate that back to a date */
		nDayCount = min->day;
		idt = min->julian;
		nYear = min->year;
		while (temp >= 0) {
			temp -= dist_weight(NULL, "calendar", nDayCount, dist + is_leap(nYear));
			nDayCount += 1;
			idt += 1;
			if (nDayCount > 365 + is_leap(nYear)) {
				nYear += 1;
				nDayCount = 1;
			}
		}
		break;
	case DIST_EXPONENTIAL:
		imean = dttoj(mean);
		imean -= idt;
	case DIST_UNIFORM:
		genrand_integer(&temp, dist, 0, range, imean, stream);
		idt += temp;
		break;
	default:
		break;
	}

	jtodt(dest, idt);

	return (0);
}

/**************
 **************
 **
 ** static routines
 **
 **************
 **************/

/*
 * Routine: init_rand()
 * Purpose: Initialize the RNG used throughout the code
 * Algorithm: To allow two columns to use the same stream of numbers (for
 *joins), pre-sort the streams list by Duplicate and then assign values. Order
 *by column after initialization Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO:
 */
// FIXME: allow re-init
void init_rand(void) {
	long long i, skip, nSeed; // changed to long long from int
	static thread_local rng_t initial_streams[MAX_COLUMN];
	static thread_local bool initial_streams_saved = false;

	if (!InitConstants::init_rand_init) {
		if (!initial_streams_saved) {
			memcpy(initial_streams, Streams, sizeof(initial_streams));
			initial_streams_saved = true;
		} else {
			memcpy(Streams, initial_streams, sizeof(initial_streams));
		}
		if (is_set("RNGSEED"))
			nSeed = get_int("RNGSEED");
		else
			nSeed = RNG_SEED;
		skip = MAXINT / MAX_COLUMN;
		for (i = 0; i < MAX_COLUMN; i++) {
            // simulate the overflow as if it were an int
            if (i != 0 && (INT_MAX - nSeed) / i < skip) {
                long long val = nSeed + skip * i;
                val %= MAXINT;
                val -= MAXINT;
                val -= 2;
                Streams[i].nInitialSeed = val;
                Streams[i].nSeed = val;
            } else {
                Streams[i].nInitialSeed = nSeed + skip * i;
                Streams[i].nSeed = nSeed + skip * i;
            }
			    Streams[i].nUsed = 0;
			    Streams[i].nTotal = 0;
		}
		InitConstants::init_rand_init = 1;
	}
	return;
}

void resetSeeds(int nTable) {
	int i;

	for (i = 0; i < MAX_COLUMN; i++)
		if (Streams[i].nTable == nTable)
			Streams[i].nSeed = Streams[i].nInitialSeed;
	return;
}

/*
 * Routine:
 * Purpose:
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
void genrand_email(char *pEmail, char *pFirst, char *pLast, int nColumn) {
	char *pDomain;
	char szCompany[50];
	int nCompanyLength;

	pick_distribution(&pDomain, "top_domains", 1, 1, nColumn);
	genrand_integer(&nCompanyLength, DIST_UNIFORM, 10, 20, 0, nColumn);
	gen_charset(&szCompany[0], ALPHANUM, 1, 20, nColumn);
	szCompany[nCompanyLength] = '\0';

	sprintf(pEmail, "%s.%s@%s.%s", pFirst, pLast, szCompany, pDomain);

	return;
}

/*
 * Routine:
 * Purpose:
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
void genrand_ipaddr(char *pDest, int nColumn) {
	int arQuads[4], i;

	for (i = 0; i < 4; i++)
		genrand_integer(&arQuads[i], DIST_UNIFORM, 1, 255, 0, nColumn);
	sprintf(pDest, "%03d.%03d.%03d.%03d", arQuads[0], arQuads[1], arQuads[2], arQuads[3]);

	return;
}

/*
 * Routine:
 * Purpose:
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int genrand_url(char *pDest, int nColumn) {
	strcpy(pDest, "http://www.foo.com");

	return (0);
}

/*
 * Routine:
 * Purpose:
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int setSeed(int nStream, int nValue) {
	int nRetValue;

	nRetValue = Streams[nStream].nSeed;
	Streams[nStream].nSeed = nValue;

	return (nRetValue);
}

#ifdef TEST
main() {
	printf("r_genrand:No test routine has been defined for this module\n");

	exit(0);
}
#endif /* TEST */
