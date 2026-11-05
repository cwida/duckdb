/* 
 * the API for FSST compression -- (c) Peter Boncz, Viktor Leis and Thomas Neumann (CWI, TU Munich), 2018-2019
 *
 * ===================================================================================================================================
 * this software is distributed under the MIT License (http://www.opensource.org/licenses/MIT):
 *
 * Copyright 2018-2020, CWI, TU Munich, FSU Jena
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files 
 * (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, 
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is 
 * furnished to do so, subject to the following conditions:
 *
 * - The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE 
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR 
 * IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * You can contact the authors via the FSST source repository : https://github.com/cwida/fsst
 * ===================================================================================================================================
 *
 * FSST: Fast Static Symbol Table compression 
 * see the paper https://github.com/cwida/fsst/raw/master/fsstcompression.pdf
 *
 * FSST is a compression scheme focused on string/text data: it can compress strings from distributions with many different values (i.e.
 * where dictionary compression will not work well). It allows *random-access* to compressed data: it is not block-based, so individual
 * strings can be decompressed without touching the surrounding data in a compressed block. When compared to e.g. lz4 (which is 
 * block-based), FSST achieves similar decompression speed, (2x) better compression speed and 30% better compression ratio on text.
 *
 * FSST encodes strings also using a symbol table -- but it works on pieces of the string, as it maps "symbols" (1-8 byte sequences) 
 * onto "codes" (single-bytes). FSST can also represent a byte as an exception (255 followed by the original byte). Hence, compression 
 * transforms a sequence of bytes into a (supposedly shorter) sequence of codes or escaped bytes. These shorter byte-sequences could 
 * be seen as strings again and fit in whatever your program is that manipulates strings.
 *
 * useful property: FSST ensures that strings that are equal, are also equal in their compressed form.
 * 
 * In this API, strings are considered byte-arrays (byte = unsigned char) and a batch of strings is represented as an array of 
 * unsigned char* pointers to their starts. A seperate length array (of unsigned int) denotes how many bytes each string consists of. 
 *
 * This representation as unsigned char* pointers tries to assume as little as possible on the memory management of the program
 * that calls this API, and is also intended to allow passing strings into this API without copying (even if you use C++ strings).
 *
 * We optionally support C-style zero-terminated strings (zero appearing only at the end). In this case, the compressed strings are 
 * also zero-terminated strings. In zero-terminated mode, the zero-byte at the end *is* counted in the string byte-length.
 */
#ifndef FSST_INCLUDED_H
#define FSST_INCLUDED_H

#if defined(_MSC_VER) && !defined(__clang__)
#define __restrict__ 
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#define __ORDER_LITTLE_ENDIAN__ 2
#include <intrin.h>
static inline int __builtin_ctzll(unsigned long long x) {
#  ifdef _WIN64
	unsigned long ret;
    _BitScanForward64(&ret, x);
	return (int)ret;
#  else
	unsigned long low, high;
	bool low_set = _BitScanForward(&low, (unsigned __int32)(x)) != 0;
	_BitScanForward(&high, (unsigned __int32)(x >> 32));
	high += 32;
	return low_set ? low : high;
#  endif
}
#endif

#ifdef __cplusplus
#define FSST_FALLTHROUGH [[fallthrough]]
#include <cstring>
extern "C" {
#else
#define FSST_FALLTHROUGH 
#endif

#ifndef __has_cpp_attribute // For backwards compatibility
#define __has_cpp_attribute(x) 0
#endif
#if __has_cpp_attribute(clang::fallthrough)
#define DUCKDB_FSST_EXPLICIT_FALLTHROUGH [[clang::fallthrough]]
#elif __has_cpp_attribute(gnu::fallthrough)
#define DUCKDB_FSST_EXPLICIT_FALLTHROUGH [[gnu::fallthrough]]
#else
#define DUCKDB_FSST_EXPLICIT_FALLTHROUGH
#endif

#include <stddef.h>

/* A compressed string is simply a string of 1-byte codes; except for code 255, which is followed by an uncompressed byte. */
#define FSST_ESC 255

/* Data structure needed for compressing strings - use duckdb_fsst_duplicate() to create thread-local copies. Use duckdb_fsst_destroy() to free. */
typedef void* duckdb_fsst_encoder_t; /* opaque type - it wraps around a rather large (~900KB) C++ object */

/* Data structure needed for decompressing strings - read-only and thus can be shared between multiple decompressing threads. */
typedef struct {
   unsigned long long version;     /* version id */
   unsigned char zeroTerminated;   /* terminator is a single-byte code that does not appear in longer symbols */
   unsigned char len[255];         /* len[x] is the byte-length of the symbol x (1 < len[x] <= 8). */
   unsigned long long symbol[255]; /* symbol[x] contains in LITTLE_ENDIAN the bytesequence that code x represents (0 <= x < 255). */ 
} duckdb_fsst_decoder_t;

/* Calibrate a FSST symboltable from a batch of strings (it is best to provide at least 16KB of data). */
duckdb_fsst_encoder_t*
duckdb_fsst_create(
   size_t n,         /* IN: number of strings in batch to sample from. */
   size_t lenIn[],   /* IN: byte-lengths of the inputs */
   unsigned char *strIn[],  /* IN: string start pointers. */
   int zeroTerminated       /* IN: whether input strings are zero-terminated. If so, encoded strings are as well (i.e. symbol[0]=""). */
);

/* Create another encoder instance, necessary to do multi-threaded encoding using the same symbol table. */ 
duckdb_fsst_encoder_t*
duckdb_fsst_duplicate(
   duckdb_fsst_encoder_t *encoder  /* IN: the symbol table to duplicate. */
);

#define FSST_MAXHEADER (8+1+8+2048+1) /* maxlen of deserialized fsst header, produced/consumed by duckdb_fsst_export() resp. duckdb_fsst_import() */

/* Space-efficient symbol table serialization (smaller than sizeof(duckdb_fsst_decoder_t) - by saving on the unused bytes in symbols of len < 8). */
unsigned int                /* OUT: number of bytes written in buf, at most sizeof(duckdb_fsst_decoder_t) */
duckdb_fsst_export(
   duckdb_fsst_encoder_t *encoder, /* IN: the symbol table to dump. */
   unsigned char *buf       /* OUT: pointer to a byte-buffer where to serialize this symbol table. */
); 

/* Deallocate encoder. */
void
duckdb_fsst_destroy(duckdb_fsst_encoder_t*);

#define DUCKDB_FSST_IMPORT_VERSION_MISMATCH 0xFFFFFFFEu
#define DUCKDB_FSST_IMPORT_OUT_OF_BOUNDS    0xFFFFFFFFu

/* Return a decoder structure from serialized format (typically used in a block-, file- or row-group header). */
unsigned int                /* OUT: bytes consumed (a symbol table was decoded), or one of the sentinels above on failure. */
duckdb_fsst_import(
   duckdb_fsst_decoder_t *decoder, /* IN: this symbol table will be overwritten. */
   const unsigned char *buf, /* IN: pointer to a byte-buffer where duckdb_fsst_export() serialized this symbol table. */
   size_t buf_size          /* IN: number of readable bytes in buf; import never reads past this. */
);

/* Return a decoder structure from an encoder. */
duckdb_fsst_decoder_t
duckdb_fsst_decoder(
   duckdb_fsst_encoder_t *encoder
);

/* Compress a batch of strings (on AVX512 machines best performance is obtained by compressing more than 32KB of string volume). */
/* The output buffer must be large; at least "conservative space" (7+2*inputlength) for the first string for something to happen. */
size_t                      /* OUT: the number of compressed strings (<=n) that fit the output buffer. */ 
duckdb_fsst_compress(
   duckdb_fsst_encoder_t *encoder, /* IN: encoder obtained from duckdb_fsst_create(). */
   size_t nstrings,         /* IN: number of strings in batch to compress. */
   size_t lenIn[],          /* IN: byte-lengths of the inputs */
   unsigned char *strIn[],  /* IN: input string start pointers. */
   size_t outsize,          /* IN: byte-length of output buffer. */
   unsigned char *output,   /* OUT: memory buffer to put the compressed strings in (one after the other). */
   size_t lenOut[],         /* OUT: byte-lengths of the compressed strings. */
   unsigned char *strOut[]  /* OUT: output string start pointers. Will all point into [output,output+size). */
);

#define DUCKDB_FSST_DECOMPRESS_INVALID_INPUT ((size_t)-1)

/* Decompress a single string, inlined for speed. An escape must be followed by a literal byte. */
/* Returns DUCKDB_FSST_DECOMPRESS_INVALID_INPUT if that byte is missing. */
inline size_t /* OUT: bytesize of the decompressed string. If > size, the decoded output is truncated to size. */
duckdb_fsst_decompress(
   duckdb_fsst_decoder_t *decoder,  /* IN: use this symbol table for compression. */
   size_t lenIn,             /* IN: byte-length of compressed string. */
   const unsigned char *strIn,     /* IN: compressed string. */
   size_t size,              /* IN: byte-length of output buffer. */
   unsigned char *output     /* OUT: memory buffer to put the decompressed string in. */
) {
   unsigned char*__restrict__ len = (unsigned char* __restrict__) decoder->len;
   unsigned char*__restrict__ strOut = (unsigned char* __restrict__) output;
   unsigned long long*__restrict__ symbol = (unsigned long long* __restrict__) decoder->symbol; 
   size_t code, posOut = 0, posIn = 0;
#ifndef FSST_MUST_ALIGN /* defining on platforms that require aligned memory access may help their performance */
#define FSST_UNALIGNED_STORE(dst,src) memcpy((void*) (dst), &(src), sizeof(unsigned long long))
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
   while (posOut+32 <= size && posIn+4 <= lenIn) {
      unsigned int nextBlock, escapeMask;
      memcpy(&nextBlock, strIn+posIn, sizeof(unsigned int));
      escapeMask = (nextBlock&0x80808080u)&((((~nextBlock)&0x7F7F7F7Fu)+0x7F7F7F7Fu)^0x80808080u);
      if (escapeMask == 0) {
         code = strIn[posIn++]; FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code]; 
         code = strIn[posIn++]; FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code]; 
         code = strIn[posIn++]; FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code]; 
         code = strIn[posIn++]; FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code]; 
     } else { 
         unsigned long firstEscapePos=static_cast<unsigned long>(__builtin_ctzll((unsigned long long) escapeMask)>>3);
         switch(firstEscapePos) { /* Duff's device */
         case 3: code = strIn[posIn++]; FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code];
			 DUCKDB_FSST_EXPLICIT_FALLTHROUGH;
         case 2: code = strIn[posIn++]; FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code];
			 DUCKDB_FSST_EXPLICIT_FALLTHROUGH;
         case 1: code = strIn[posIn++]; FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code];
			 DUCKDB_FSST_EXPLICIT_FALLTHROUGH;
         case 0:
             if (posIn+1 >= lenIn) {
                return DUCKDB_FSST_DECOMPRESS_INVALID_INPUT;
             }
             posIn+=2; strOut[posOut++] = strIn[posIn-1]; /* decompress an escaped byte */
         }
      }
   }
   if (posOut+32 <= size) { // handle the possibly 3 last bytes without a loop
      if (posIn+2 <= lenIn) { 
	 strOut[posOut] = strIn[posIn+1]; 
         if (strIn[posIn] != FSST_ESC) {
            code = strIn[posIn++]; FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code]; 
            if (strIn[posIn] != FSST_ESC) {
               code = strIn[posIn++]; FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code]; 
            } else {
               if (posIn+1 >= lenIn) {
                  return DUCKDB_FSST_DECOMPRESS_INVALID_INPUT;
               }
               posIn += 2; strOut[posOut++] = strIn[posIn-1]; 
            }
         } else {
            posIn += 2; posOut++; 
         } 
      }
      if (posIn < lenIn) { // last code cannot be an escape
         code = strIn[posIn++];
         if (code == FSST_ESC) {
            return DUCKDB_FSST_DECOMPRESS_INVALID_INPUT;
         }
         FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); posOut += len[code];
      }
   }
#else
   while (posOut+8 <= size && posIn < lenIn)
      if ((code = strIn[posIn++]) < FSST_ESC) { /* symbol compressed as code? */
         FSST_UNALIGNED_STORE(strOut+posOut, symbol[code]); /* unaligned memory write */
         posOut += len[code];
      } else { 
         if (posIn == lenIn) {
            return DUCKDB_FSST_DECOMPRESS_INVALID_INPUT;
         }
         strOut[posOut] = strIn[posIn]; /* decompress an escaped byte */
         posIn++; posOut++; 
      }
#endif
#endif
   while (posIn < lenIn)
      if ((code = strIn[posIn++]) < FSST_ESC) {
         size_t posWrite = posOut, endWrite = posOut + len[code], symbolOffset = 0;
         unsigned char* __restrict__ symbolPointer = (unsigned char* __restrict__) &symbol[code];
         if ((posOut = endWrite) > size) endWrite = size;
         for(; posWrite < endWrite; posWrite++)  /* only write if there is room */
            strOut[posWrite] = symbolPointer[symbolOffset++];
      } else {
         if (posIn == lenIn) {
            return DUCKDB_FSST_DECOMPRESS_INVALID_INPUT;
         }
         if (posOut < size) strOut[posOut] = strIn[posIn]; /* idem */
         posIn++; posOut++; 
      } 
   if (size && posOut >= size && (decoder->zeroTerminated&1)) strOut[size-1] = 0;
   return posOut; /* full size of decompressed string (could be >size, then the actually decompressed part) */
}

#ifdef __cplusplus
}
#endif
#endif /* FSST_INCLUDED_H */
