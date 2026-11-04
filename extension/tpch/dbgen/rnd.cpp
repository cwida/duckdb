/*
 * Copyright owned by the Transaction Processing Performance Council.
 *
 * A copy of the license is included under extension/tpch/dbgen/LICENSE
 * in this repository.
 *
 * You may not use this file except in compliance with the License.
 *
 * THE TPC SOFTWARE IS AVAILABLE WITHOUT CHARGE FROM TPC.
 *//*
 * RANDOM.C -- Implements Park & Miller's "Minimum Standard" RNG
 *
 * (Reference:  CACM, Oct 1988, pp 1192-1201)
 *
 * NextRand:  Computes next random integer
 * UnifInt:   Yields an long uniformly distributed between given bounds
 * UnifReal: ields a real uniformly distributed between given bounds
 * Exponential: Yields a real exponentially distributed with given mean
 *
 */

#include "dbgen/config.h"

#include "duckdb/common/assert.hpp"

#include <limits>

#include <array>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "dbgen/dss.h"
#include "dbgen/rnd.h"

void NthElement(DSS_HUGE, DSS_HUGE *);

void dss_random(DSS_HUGE *tgt, DSS_HUGE lower, DSS_HUGE upper, seed_t *seed) {
	*tgt = UnifInt(lower, upper, seed);
	seed->usage += 1;

	return;
}

static int seed_table(int t) {
	if (t == ORDER_LINE) {
		return ORDER;
	}
	if (t == PART_PSUPP) {
		return PART;
	}
	return t;
}

static bool seed_matches_table(seed_t &seed, int t, DBGenContext *ctx) {
	return seed.table == t || seed.table == ctx->tdefs[t].child;
}

// the streams of each table; the seed table and the child relation are constants of the generator
struct TableStreams {
	int count = 0;
	int stream[MAX_STREAM + 1];
};

static constexpr int TABLE_COUNT = sizeof(DBGenContext::tdefs) / sizeof(DBGenContext::tdefs[0]);

static const TableStreams &table_streams(int t, DBGenContext *ctx) {
	static const std::array<TableStreams, TABLE_COUNT> streams = [ctx]() {
		std::array<TableStreams, TABLE_COUNT> result;
		for (int table = 0; table < TABLE_COUNT; table++) {
			for (int i = 0; i <= MAX_STREAM; i++) {
				if (seed_matches_table(ctx->Seed[i], table, ctx)) {
					result[table].stream[result[table].count++] = i;
				}
			}
		}
		return result;
	}();
	D_ASSERT(t >= 0 && t < TABLE_COUNT);
	return streams[t];
}

void row_start(int t, DBGenContext *ctx) {
	auto &streams = table_streams(seed_table(t), ctx);
	for (int s = 0; s < streams.count; s++) {
		ctx->Seed[streams.stream[s]].usage = 0;
	}
}

void row_stop_h(int t, DBGenContext *ctx) {
	auto &streams = table_streams(seed_table(t), ctx);
	for (int s = 0; s < streams.count; s++) {
		auto &seed = ctx->Seed[streams.stream[s]];
		if (set_seeds && (seed.usage > seed.boundary)) {
			fprintf(stderr, "\nSEED CHANGE: seed[%d].usage = " HUGE_FORMAT "\n", streams.stream[s], seed.usage);
			seed.boundary = seed.usage;
		} else {
			auto advance_count = seed.boundary - seed.usage;
			if (advance_count > 0) {
				NthElement(advance_count, &seed.value);
			}
#ifdef RNG_TEST
			seed.nCalls += advance_count;
#endif
		}
	}
}

void dump_seeds(int tbl, seed_t *seeds) {
	int i;

	for (i = 0; i <= MAX_STREAM; i++)
		if (seeds[i].table == tbl)
#ifdef RNG_TEST
			printf("%d(" HUGE_FORMAT "):\t" HUGE_FORMAT "\n", i, seeds[i].nCalls, seeds[i].value);
#else
			printf("%d:\t" HUGE_FORMAT "\n", i, seeds[i].value);
#endif
	return;
}

/******************************************************************

   NextRand:  Computes next random integer

*******************************************************************/

/*
 * long NextRand( long nSeed )
 */
DSS_HUGE
NextRand(DSS_HUGE nSeed)

/*
 * nSeed is the previous random number; the returned value is the
 * next random number. The routine generates all numbers in the
 * range 1 .. nM-1.
 */

{
	nSeed = (nSeed * 16807) % 2147483647;
	return (nSeed);
}

/******************************************************************

   UnifInt:  Yields an long uniformly distributed between given bounds

*******************************************************************/

/*
 * long UnifInt( long nLow, long nHigh, seed_t *seed )
 */
DSS_HUGE
UnifInt(DSS_HUGE nLow, DSS_HUGE nHigh, seed_t *seed)

/*
 * Returns an integer uniformly distributed between nLow and nHigh,
 * including * the endpoints. Seed points to the random number stream.
 */

{
	double dRange;
	DSS_HUGE nTemp;
	int32_t nLow32 = (int32_t)nLow, nHigh32 = (int32_t)nHigh;

	if ((nHigh == MAX_LONG) && (nLow == 0)) {
		// the reference dbgen overflows int32 here; replicate it to stay bit-compatible
		dRange = (double)std::numeric_limits<int32_t>::min();
	} else {
		dRange = (double)(nHigh - nLow + 1);
	}

	seed->value = NextRand(seed->value);
#ifdef RNG_TEST
	seed->nCalls += 1;
#endif
	nTemp = (DSS_HUGE)(((double)seed->value / DBGenContext::dM) * (dRange));
	return (nLow + nTemp);
}
