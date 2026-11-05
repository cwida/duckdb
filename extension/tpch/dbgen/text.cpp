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
/*
 * text.c --- pseaudo text generator for use in DBGEN 2.0
 *
 * Defined Routines:
 *		dbg_text() -- select and translate a sentance form
 */

#ifdef TEXT_TEST
#define DECLARER
#endif /* TEST */

#include "dbgen/config.h"

#include <stdlib.h>
#ifndef _WIN32
 /* Change for Windows NT */
#include <unistd.h>
#endif /* _WIN32 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h> /* */
#include <string.h>
#ifdef HP
#include <strings.h>
#endif
#if (defined(WIN32) && !defined(_POSIX_))
#include <process.h>
#pragma warning(disable : 4201)
#pragma warning(disable : 4214)
#pragma warning(disable : 4514)
#define WIN32_LEAN_AND_MEAN
#define NOATOM
#define NOGDICAPMASKS
#define NOMETAFILE
#define NOMINMAX
#define NOMSG
#define NOOPENFILE
#define NORASTEROPS
#define NOSCROLL
#define NOSOUND
#define NOSYSMETRICS
#define NOTEXTMETRIC
#define NOWH
#define NOCOMM
#define NOKANJI
#define NOMCX
#include <windows.h>
#pragma warning(default : 4201)
#pragma warning(default : 4214)
#endif

#include "dbgen/dss.h"
#include "dbgen/dsstypes.h"
#include "dbgen/text_pool_checkpoints.h"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/storage/storage_info.hpp"

#include <atomic>
#include <thread>

using duckdb::idx_t;
using duckdb::InternalException;
using duckdb::MinValue;

/*
 * txt_vp() --
 *		generate a verb phrase by
 *		1) selecting a verb phrase form
 *		2) parsing it to select parts of speech
 *		3) selecting appropriate words
 *		4) adding punctuation as required
 *
 *	Returns: length of generated phrase
 *	Called By: txt_sentence()
 *	Calls: pick_str()
 */
static int txt_vp(char *dest, seed_t *seed) {
	char syntax[MAX_GRAMMAR_LEN + 1], *cptr, *parse_target;
	distribution *src;
	int i, res = 0;

	pick_str(&vp, seed, &syntax[0]);
	parse_target = syntax;
	while ((cptr = strtok(parse_target, " ")) != NULL) {
		src = NULL;
		switch (*cptr) {
		case 'D':
			src = &adverbs;
			break;
		case 'V':
			src = &verbs;
			break;
		case 'X':
			src = &auxillaries;
			break;
		} /* end of POS switch statement */
		i = pick_str(src, seed, dest);
		i = (int)strlen(DIST_MEMBER(src, i));
		dest += i;
		res += i;
		if (*(++cptr)) /* miscelaneous fillagree, like punctuation */
		{
			dest += 1;
			res += 1;
			*dest = *cptr;
		}
		*dest = ' ';
		dest++;
		res++;
		parse_target = NULL;
	} /* end of while loop */

	return (res);
}

/*
 * txt_np() --
 *		generate a noun phrase by
 *		1) selecting a noun phrase form
 *		2) parsing it to select parts of speech
 *		3) selecting appropriate words
 *		4) adding punctuation as required
 *
 *	Returns: length of generated phrase
 *	Called By: txt_sentence()
 *	Calls: pick_str(),
 */
static int txt_np(char *dest, seed_t *seed) {
	char syntax[MAX_GRAMMAR_LEN + 1], *cptr, *parse_target;
	distribution *src;
	int i, res = 0;

	pick_str(&np, seed, &syntax[0]);
	parse_target = syntax;
	while ((cptr = strtok(parse_target, " ")) != NULL) {
		src = NULL;
		switch (*cptr) {
		case 'A':
			src = &articles;
			break;
		case 'J':
			src = &adjectives;
			break;
		case 'D':
			src = &adverbs;
			break;
		case 'N':
			src = &nouns;
			break;
		} /* end of POS switch statement */
		i = pick_str(src, seed, dest);
		i = (int)strlen(DIST_MEMBER(src, i));
		dest += i;
		res += i;
		if (*(++cptr)) /* miscelaneous fillagree, like punctuation */
		{
			*dest = *cptr;
			dest += 1;
			res += 1;
		}
		*dest = ' ';
		dest++;
		res++;
		parse_target = NULL;
	} /* end of while loop */

	return (res);
}

/*
 * txt_sentence() --
 *		generate a sentence by
 *		1) selecting a sentence form
 *		2) parsing it to select parts of speech or phrase types
 *		3) selecting appropriate words
 *		4) adding punctuation as required
 *
 *	Returns: length of generated sentence
 *	Called By: dbg_text()
 *	Calls: pick_str(), txt_np(), txt_vp()
 */
static int txt_sentence(char *dest, seed_t *seed) {
	char syntax[MAX_GRAMMAR_LEN + 1], *cptr;
	int i, res = 0, len = 0;

	pick_str(&grammar, seed, syntax);
	cptr = syntax;

next_token: /* I hate goto's, but can't seem to have parent and child use strtok() */
	while (*cptr && *cptr == ' ')
		cptr++;
	if (*cptr == '\0')
		goto done;
	switch (*cptr) {
	case 'V':
		len = txt_vp(dest, seed);
		break;
	case 'N':
		len = txt_np(dest, seed);
		break;
	case 'P':
		i = pick_str(&prepositions, seed, dest);
		len = (int)strlen(DIST_MEMBER(&prepositions, i));
		strcpy((dest + len), " the ");
		len += 5;
		len += txt_np(dest + len, seed);
		break;
	case 'T':
		i = pick_str(&terminators, seed, --dest); /*terminators should abut previous word */
		len = (int)strlen(DIST_MEMBER(&terminators, i));
		break;
	} /* end of POS switch statement */
	dest += len;
	res += len;
	cptr++;
	if (*cptr && *cptr != ' ') /* miscelaneous fillagree, like punctuation */
	{
		dest += 1;
		res += 1;
		*dest = *cptr;
	}
	goto next_token;
done:
	*dest = '\0';
	return (--res);
}

static char *gen_text(char *dest, seed_t *seed, distribution *s) {
	long i = 0;
	DSS_HUGE j;

	RANDOM(j, 1, s->list[s->count - 1].weight, seed);
	while (s->list[i].weight < j)
		i++;
	char *src = s->list[i].text;
	int ind = 0;
	while (src[ind]) {
		dest[ind] = src[ind];
		ind++;
	}
	dest[ind] = ' ';
	return dest + ind + 1;
}

#define NOUN_MAX_WEIGHT 340
#define ADJECTIVES_MAX_WEIGHT 289
#define ADVERBS_MAX_WEIGHT 262
#define AUXILLARIES_MAX_WEIGHT 18
#define VERBS_MAX_WEIGHT 174
#define PREPOSITIONS_MAX_WEIGHT 456

static char *noun_index[NOUN_MAX_WEIGHT + 1];
static char *adjectives_index[ADJECTIVES_MAX_WEIGHT + 1];
static char *adverbs_index[ADVERBS_MAX_WEIGHT + 1];
static char *auxillaries_index[AUXILLARIES_MAX_WEIGHT + 1];
static char *verbs_index[VERBS_MAX_WEIGHT + 1];
static char *prepositions_index[PREPOSITIONS_MAX_WEIGHT + 1];

// generate a lookup table for weight -> str
static void gen_index(char **index, distribution *s) {
	for (size_t w = 0; w <= s->list[s->count - 1].weight; w++) {
		long i = 0;
		while (s->list[i].weight < w)
			i++;
		index[w] = s->list[i].text;
	}
}

static char *gen_text_index(char *dest, seed_t *seed, char **index, distribution *s) {
	DSS_HUGE j;

	RANDOM(j, 1, s->list[s->count - 1].weight, seed);
	char *src = index[j];
	int ind = 0;
	while (src[ind]) {
		dest[ind] = src[ind];
		ind++;
	}
	dest[ind] = ' ';
	return dest + ind + 1;
}

static char *gen_vp(char *dest, seed_t *seed) {
	DSS_HUGE j;
	RANDOM(j, 1, vp.list[vp.count - 1].weight, seed);
	int index = 0;
	index += vp.list[0].weight < j;
	index += vp.list[1].weight < j;
	index += vp.list[2].weight < j;

	if (index == 0) {
		dest = gen_text_index(dest, seed, verbs_index, &verbs);
	} else if (index == 1) {
		dest = gen_text_index(dest, seed, auxillaries_index, &auxillaries);
		dest = gen_text_index(dest, seed, verbs_index, &verbs);
	} else if (index == 2) {
		dest = gen_text_index(dest, seed, verbs_index, &verbs);
		dest = gen_text_index(dest, seed, adverbs_index, &adverbs);
	} else {
		dest = gen_text_index(dest, seed, auxillaries_index, &auxillaries);
		dest = gen_text_index(dest, seed, verbs_index, &verbs);
		dest = gen_text_index(dest, seed, adverbs_index, &adverbs);
	}
	return dest;
}

static char *gen_np(char *dest, seed_t *seed) {
	DSS_HUGE j;
	RANDOM(j, 1, np.list[np.count - 1].weight, seed);
	int index = 0;
	index += np.list[0].weight < j;
	index += np.list[1].weight < j;
	index += np.list[2].weight < j;

	if (index == 0) {
		dest = gen_text_index(dest, seed, noun_index, &nouns);
	} else if (index == 1) {
		dest = gen_text_index(dest, seed, adjectives_index, &adjectives);
		dest = gen_text_index(dest, seed, noun_index, &nouns);
	} else if (index == 2) {
		dest = gen_text_index(dest, seed, adjectives_index, &adjectives);
		dest[-1] = ',';
		*(dest++) = ' ';
		dest = gen_text_index(dest, seed, adjectives_index, &adjectives);
		dest = gen_text_index(dest, seed, noun_index, &nouns);
	} else {
		dest = gen_text_index(dest, seed, adverbs_index, &adverbs);
		dest = gen_text_index(dest, seed, adjectives_index, &adjectives);
		dest = gen_text_index(dest, seed, noun_index, &nouns);
	}
	return dest;
}

static char *gen_preposition(char *dest, seed_t *seed) {
	dest = gen_text_index(dest, seed, prepositions_index, &prepositions);
	*(dest++) = 't';
	*(dest++) = 'h';
	*(dest++) = 'e';
	*(dest++) = ' ';
	return gen_np(dest, seed);
}

static char *gen_terminator(char *dest, seed_t *seed) {
	dest = gen_text(--dest, seed, &terminators);
	return dest - 1;
}

static char *gen_sentence(char *dest, seed_t *seed) {
	const char *cptr;
	int i;

	DSS_HUGE j;
	RANDOM(j, 1, grammar.list[grammar.count - 1].weight, seed);
	int index = 0;
	index += grammar.list[0].weight < j;
	index += grammar.list[1].weight < j;
	index += grammar.list[2].weight < j;
	index += grammar.list[3].weight < j;
	cptr = grammar.list[index].text;

	if (index == 0) {
		dest = gen_np(dest, seed);
		dest = gen_vp(dest, seed);
		dest = gen_terminator(dest, seed);
	} else if (index == 1) {
		dest = gen_np(dest, seed);
		dest = gen_vp(dest, seed);
		dest = gen_preposition(dest, seed);
		dest = gen_terminator(dest, seed);
	} else if (index == 2) {
		dest = gen_np(dest, seed);
		dest = gen_vp(dest, seed);
		dest = gen_np(dest, seed);
		dest = gen_terminator(dest, seed);
	} else if (index == 3) {
		dest = gen_np(dest, seed);
		dest = gen_preposition(dest, seed);
		dest = gen_vp(dest, seed);
		dest = gen_np(dest, seed);
		dest = gen_terminator(dest, seed);
	} else {
		dest = gen_np(dest, seed);
		dest = gen_preposition(dest, seed);
		dest = gen_vp(dest, seed);
		dest = gen_preposition(dest, seed);
		dest = gen_terminator(dest, seed);
	}
	*dest = ' ';
	return dest + 1;
}

/*
 * The text pool is 300MiB of pseudo-text in one contiguous arena, split into chunks that are generated in
 * place on first use. Every chunk starts from a precomputed checkpoint (sentence offset + RNG state), so
 * chunks are independent of each other and can be generated in parallel by the dbgen worker threads.
 * Untouched chunks cost no physical memory.
 */
static_assert(TEXT_POOL_SIZE == ((long)TEXT_POOL_CHUNK_COUNT << TEXT_POOL_CHUNK_SHIFT),
              "text pool checkpoints do not match TEXT_POOL_SIZE");
static_assert((idx_t(1) << TEXT_POOL_CHUNK_SHIFT) == DEFAULT_BLOCK_ALLOC_SIZE,
              "a text pool chunk is sized like a memory block");

enum class TextPoolChunkState : uint8_t { EMPTY = 0, GENERATING = 1, READY = 2 };

static char *text_pool = nullptr;
static std::atomic<TextPoolChunkState> text_pool_chunk_state[TEXT_POOL_CHUNK_COUNT];
static long txtBufferSize = 0;

// generate the bytes [chunk_start, chunk_end) in place
static void gen_text_pool_chunk(idx_t chunk) {
	auto &start_checkpoint = TEXT_POOL_CHECKPOINTS[chunk];
	auto &end_checkpoint = TEXT_POOL_CHECKPOINTS[chunk + 1];
	const idx_t chunk_start = chunk << TEXT_POOL_CHUNK_SHIFT;
	const idx_t chunk_end = chunk_start + (idx_t(1) << TEXT_POOL_CHUNK_SHIFT);

	seed_t seed = {};
	seed.value = start_checkpoint.seed;
	char sentence[TEXT_POOL_MAX_SENTENCE_LEN + 1];

	idx_t pos = start_checkpoint.offset;
	idx_t sentence_start = pos;
	DSS_HUGE sentence_seed = seed.value;
	while (pos < chunk_end) {
		// the sentence covering the chunk end must match the next checkpoint
		sentence_start = pos;
		sentence_seed = seed.value;
		auto length = idx_t(gen_sentence(sentence, &seed) - sentence);
		D_ASSERT(length <= TEXT_POOL_MAX_SENTENCE_LEN);
		// copy the part of the sentence that falls inside this chunk
		idx_t copy_begin = pos < chunk_start ? chunk_start - pos : 0;
		idx_t copy_end = MinValue<idx_t>(length, chunk_end - pos);
		if (copy_begin < copy_end) {
			memcpy(text_pool + (pos + copy_begin), sentence + copy_begin, copy_end - copy_begin);
		}
		pos += length;
	}
	if (pos == chunk_end) {
		// the next chunk starts exactly on a sentence boundary
		sentence_start = pos;
		sentence_seed = seed.value;
	}
	if (end_checkpoint.offset != sentence_start || (DSS_HUGE)end_checkpoint.seed != sentence_seed) {
		throw InternalException("TPC-H text pool checkpoint %llu does not match the generated text", chunk + 1);
	}
}

static void ensure_text_pool_chunk(idx_t chunk) {
	auto &state = text_pool_chunk_state[chunk];
	auto current = state.load(std::memory_order_acquire);
	if (current == TextPoolChunkState::READY) {
		return;
	}
	if (current == TextPoolChunkState::EMPTY &&
	    state.compare_exchange_strong(current, TextPoolChunkState::GENERATING, std::memory_order_acq_rel)) {
		try {
			gen_text_pool_chunk(chunk);
		} catch (...) {
			state.store(TextPoolChunkState::EMPTY, std::memory_order_release);
			throw;
		}
		state.store(TextPoolChunkState::READY, std::memory_order_release);
		return;
	}
	// another thread is generating this chunk
	while (state.load(std::memory_order_acquire) != TextPoolChunkState::READY) {
		std::this_thread::yield();
	}
}

/*
 * init_text_pool() --
 *    reserve the text pool; chunks are generated lazily by dbg_text_source().
 */
void init_text_pool(long bSize, DBGenContext *ctx) {
	if (bSize != TEXT_POOL_SIZE) {
		throw InternalException("TPC-H text pool must be %ld bytes", (long)TEXT_POOL_SIZE);
	}
	if ((DSS_HUGE)TEXT_POOL_CHECKPOINTS[0].seed != ctx->Seed[5].value || TEXT_POOL_CHECKPOINTS[0].offset != 0) {
		throw InternalException("TPC-H text pool checkpoints do not match the text seed");
	}
	gen_index(noun_index, &nouns);
	gen_index(adjectives_index, &adjectives);
	gen_index(adverbs_index, &adverbs);
	gen_index(auxillaries_index, &auxillaries);
	gen_index(verbs_index, &verbs);
	gen_index(prepositions_index, &prepositions);

	D_ASSERT(!text_pool);
	text_pool = (char *)malloc(TEXT_POOL_SIZE);
	if (!text_pool) {
		throw std::bad_alloc();
	}
	for (auto &state : text_pool_chunk_state) {
		state.store(TextPoolChunkState::EMPTY, std::memory_order_relaxed);
	}
	txtBufferSize = bSize;
}

void free_text_pool() {
	free(text_pool);
	text_pool = nullptr;
	txtBufferSize = 0;
}

/*
 * dbg_text_source() --
 *		select a random slice of the text pool; the result stays valid until free_text_pool()
 */
int dbg_text_source(int min, int max, seed_t *seed, const char **source) {
	DSS_HUGE hgLength = 0, hgOffset;
	RANDOM(hgOffset, 0, txtBufferSize - max, seed);
	RANDOM(hgLength, min, max, seed);
	D_ASSERT(hgLength > 0 && hgLength < (1 << TEXT_POOL_CHUNK_SHIFT));
	auto first_chunk = idx_t(hgOffset) >> TEXT_POOL_CHUNK_SHIFT;
	auto last_chunk = idx_t(hgOffset + hgLength - 1) >> TEXT_POOL_CHUNK_SHIFT;
	ensure_text_pool_chunk(first_chunk);
	if (last_chunk != first_chunk) {
		ensure_text_pool_chunk(last_chunk);
	}
	*source = text_pool + hgOffset;
#ifndef _MSC_VER
	// the bytes are copied out later, when the chunk is appended; warm the cache lines now
	for (auto line = *source; line < *source + hgLength; line += 64) {
		__builtin_prefetch(line);
	}
#endif
	return (int)hgLength;
}

void dbg_text(char *tgt, int min, int max, seed_t *seed) {
	const char *source;
	auto hgLength = dbg_text_source(min, max, seed, &source);
	memcpy(&tgt[0], source, (size_t)hgLength);
	tgt[hgLength] = '\0';
}
