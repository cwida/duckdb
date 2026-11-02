/*
 * Copyright owned by the Transaction Processing Performance Council.
 *
 * A copy of the license is included under extension/tpch/dbgen/LICENSE
 * in this repository.
 *
 * You may not use this file except in compliance with the License.
 *
 * THE TPC SOFTWARE IS AVAILABLE WITHOUT CHARGE FROM TPC.
 */
#include "dbgen/config.h"

#include <array>
#include "dbgen/dss.h"
#include "dbgen/rng64.h"

#include <stdio.h>
#include <stdlib.h>

/*  _tal long RandSeed = "Random^SeedFromTimestamp" (void); */

#define ADVANCE_STREAM(stream, num_calls)   advanceStream(stream, num_calls, 0)
#define ADVANCE_STREAM64(stream, num_calls) advanceStream(stream, num_calls, 1)
#define MAX_COLOR                              92
long name_bits[MAX_COLOR / BITS_PER_LONG];
void fakeVStr(int nAvg, long nSeed, DSS_HUGE nCount);
void NthElement(DSS_HUGE N, DSS_HUGE *StartSeed);

void advanceStream(seed_t *seed, DSS_HUGE nCalls, int bUse64Bit) {
	if (bUse64Bit)
		seed->value = AdvanceRand64(seed->value, nCalls);
	else
		NthElement(nCalls, &seed->value);

#ifdef RNG_TEST
	seed->nCalls += nCalls;
#endif

	return;
}

/* WARNING!  This routine assumes the existence of 64-bit                 */
/* integers.  The notation used here- "HUGE" is *not* ANSI standard. */
/* Hopefully, you have this extension as well.  If not, use whatever      */
/* nonstandard trick you need to in order to get 64 bit integers.         */
/* The book says that this will work if MAXINT for the type you choose    */
/* is at least 2**46  - 1, so 64 bits is more than you *really* need      */

static DSS_HUGE Multiplier = 16807;   /* or whatever nonstandard */
static DSS_HUGE Modulus = 2147483647; /* trick you use to get 64 bit int */

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
void NthElement(DSS_HUGE N, DSS_HUGE *StartSeed) {
	DSS_HUGE Z;
	DSS_HUGE Mult;
	static int ln = -1;
	int i;

	if ((verbose > 0) && ++ln % 1000 == 0) {
		i = ln % LN_CNT;
		fprintf(stderr, "%c\b", lnoise[i]);
	}
	Z = (DSS_HUGE)*StartSeed;
	// small advances happen on every row (row_stop_h); use a table of Multiplier^N
	static const auto small_powers = []() {
		std::array<DSS_HUGE, 256> powers;
		powers[0] = 1;
		for (size_t i = 1; i < powers.size(); i++) {
			powers[i] = (powers[i - 1] * Multiplier) % Modulus;
		}
		return powers;
	}();
	if (N >= 0 && N < (DSS_HUGE)small_powers.size()) {
		*StartSeed = (small_powers[(size_t)N] * Z) % Modulus;
		return;
	}
	Mult = Multiplier;
	while (N > 0) {
		if (N % 2 != 0) /* testing for oddness, this seems portable */
			Z = (Mult * Z) % Modulus;
		N = N / 2; /* integer division, truncates */
		Mult = (Mult * Mult) % Modulus;
	}
	*StartSeed = Z;

	return;
}

/* updates 'seed' using the tpch_a_rnd algorithm */
void fake_tpch_a_rnd(int min, int max, seed_t *seed) {
	DSS_HUGE len;
	DSS_HUGE itcount;

	RANDOM(len, min, max, seed);
	if (len % 5L == 0)
		itcount = len / 5;
	else
		itcount = len / 5 + 1L;
	NthElement(itcount, &seed->usage);
#ifdef RNG_TEST
	seed->nCalls += itcount;
#endif
	return;
}

long sd_part(int child, DSS_HUGE skip_count, DBGenContext *ctx) {
	(void)child;
	int i;

	for (i = P_MFG_SD; i <= P_CNTR_SD; i++)
		ADVANCE_STREAM(&ctx->Seed[i], skip_count);

	ADVANCE_STREAM(&ctx->Seed[P_CMNT_SD], skip_count * 2);
	ADVANCE_STREAM(&ctx->Seed[P_NAME_SD], skip_count * 92);

	return (0L);
}

long sd_line(int child, DSS_HUGE skip_count, DBGenContext *ctx) {
	int i, j;

	for (j = 0; j < O_LCNT_MAX; j++) {
		for (i = L_QTY_SD; i <= L_RFLG_SD; i++)
			/*
			            if (ctx->scale_factor >= 30000 && i == L_PKEY_SD)
			                ADVANCE_STREAM64(&ctx->Seed[i], skip_count);
			            else
			*/
			ADVANCE_STREAM(&ctx->Seed[i], skip_count);
		ADVANCE_STREAM(&ctx->Seed[L_CMNT_SD], skip_count * 2);
	}

	/* need to special case this as the link between master and detail */
	if (child == 1) {
		ADVANCE_STREAM(&ctx->Seed[O_ODATE_SD], skip_count);
		ADVANCE_STREAM(&ctx->Seed[O_LCNT_SD], skip_count);
	}

	return (0L);
}

long sd_order(int child, DSS_HUGE skip_count, DBGenContext *ctx) {
	(void)child;
	ADVANCE_STREAM(&ctx->Seed[O_LCNT_SD], skip_count);
	/*
	    if (ctx->scale_factor >= 30000)
	        ADVANCE_STREAM64(&ctx->Seed[O_CKEY_SD], skip_count);
	    else
	*/
	ADVANCE_STREAM(&ctx->Seed[O_CKEY_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[O_CMNT_SD], skip_count * 2);
	ADVANCE_STREAM(&ctx->Seed[O_SUPP_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[O_CLRK_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[O_PRIO_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[O_ODATE_SD], skip_count);

	return (0L);
}

long sd_psupp(int child, DSS_HUGE skip_count, DBGenContext *ctx) {
	(void)child;

	int j;

	for (j = 0; j < SUPP_PER_PART; j++) {
		ADVANCE_STREAM(&ctx->Seed[PS_QTY_SD], skip_count);
		ADVANCE_STREAM(&ctx->Seed[PS_SCST_SD], skip_count);
		ADVANCE_STREAM(&ctx->Seed[PS_CMNT_SD], skip_count * 2);
	}

	return (0L);
}

long sd_cust(int child, DSS_HUGE skip_count, DBGenContext *ctx) {
	(void)child;

	ADVANCE_STREAM(&ctx->Seed[C_ADDR_SD], skip_count * 9);
	ADVANCE_STREAM(&ctx->Seed[C_CMNT_SD], skip_count * 2);
	ADVANCE_STREAM(&ctx->Seed[C_NTRG_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[C_PHNE_SD], 3L * skip_count);
	ADVANCE_STREAM(&ctx->Seed[C_ABAL_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[C_MSEG_SD], skip_count);
	return (0L);
}

long sd_supp(int child, DSS_HUGE skip_count, DBGenContext *ctx) {
	(void)child;

	ADVANCE_STREAM(&ctx->Seed[S_NTRG_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[S_PHNE_SD], 3L * skip_count);
	ADVANCE_STREAM(&ctx->Seed[S_ABAL_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[S_ADDR_SD], skip_count * 9);
	ADVANCE_STREAM(&ctx->Seed[S_CMNT_SD], skip_count * 2);
	ADVANCE_STREAM(&ctx->Seed[BBB_CMNT_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[BBB_JNK_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[BBB_OFFSET_SD], skip_count);
	ADVANCE_STREAM(&ctx->Seed[BBB_TYPE_SD], skip_count); /* avoid one trudge */

	return (0L);
}

long sd_nation(int child, DSS_HUGE skip_count, DBGenContext *ctx) {
	(void)child;

	ADVANCE_STREAM(&ctx->Seed[N_CMNT_SD], skip_count * 2);

	return (0L);
}

long sd_region(int child, DSS_HUGE skip_count, DBGenContext *ctx) {
	(void)child;

	ADVANCE_STREAM(&ctx->Seed[R_CMNT_SD], skip_count * 2);

	return (0L);
}
