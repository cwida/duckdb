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
#ifndef PORTING_H
#define PORTING_H

#include <string.h>
//#ifdef USE_STRING_H
//#include <string.h>
//#else
//#include <strings.h>
//#endif

#ifdef USE_VALUES_H
#include <values.h>
#endif

#ifdef USE_LIMITS_H
#include <limits.h>
#endif

#ifdef USE_STDLIB_H
#include <stdlib.h>
#endif

#include <stdint.h>

#ifdef _WIN32
#include <sys/timeb.h>
#define timeb _timeb
#define ftime _ftime
#else
#include <sys/time.h>
#endif

#include "config.h"

typedef HUGE_TYPE ds_key_t;

/*
 * add some functions that are not strictly ANSI standard
 */
#ifndef strdup
char *strdup(const char *);
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <winbase.h>
#include <io.h>
#define random                rand
#define strncasecmp           _strnicmp
#define strcasecmp            _stricmp
#define strdup                _strdup
#define access                _access
#define isatty                _isatty
#define fileno                _fileno
#define F_OK                  0
#define MAXINT                INT_MAX
#define THREAD                __declspec(thread)
#define MIN_MULTI_NODE_ROWS   100000
#define MIN_MULTI_THREAD_ROWS 5000
#define THREAD                __declspec(thread)
/* Lines added by Chuck McDevitt for WIN32 support */
#ifndef _POSIX_
#ifndef S_ISREG
#define S_ISREG(m)  (((m)&_S_IFMT) == _S_IFREG)
#define S_ISFIFO(m) (((m)&_S_IFMT) == _S_IFIFO)
#endif
#endif
#endif /* WIN32 */

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef INTERIX
#include <limits.h>
#define MAXINT INT_MAX
#endif /* INTERIX */

#ifdef AIX
#define MAXINT INT_MAX
#endif

#ifdef MACOS
#include <limits.h>
#define MAXINT INT_MAX
#endif /* MACOS */

#include "duckdb/common/exception.hpp"

#define INTERNAL(m) throw duckdb::InternalException("dsdgen: %s (%s:%d)", m, __FILE__, __LINE__)

#define MALLOC_CHECK(v)                                                                                                \
	if (v == NULL) {                                                                                                   \
		throw duckdb::OutOfMemoryException("dsdgen: malloc failed at %s:%d", __FILE__, __LINE__);                      \
	}
#endif
