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
 *
 * Various routines that handle distributions, value selections and
 * seed value management for the DSS benchmark. Current functions:
 * tpch_a_rnd(min, max) -- random alphanumeric within length range
 * pick_str(size, set) -- select a string from the set of size
 * read_dist(name, distribution *) -- read named dist from the embedded dists
 * tbl_open(path, mode) -- std fopen with lifenoise
 * julian(date) -- julian date correction
 * rowcnt(tbl) -- proper scaling of given table
 * e_str(set, min, max) -- build an embedded str
 * agg_str() -- build a string from the named set
 * dsscasecmp() -- version of strcasecmp()
 * dssncasecmp() -- version of strncasecmp()
 * getopt()
 * set_state() -- initialize the RNG
 */

#include "dbgen/config.h"
#include "dbgen/dss.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef HP
#include <strings.h>
#endif /* HP */
#include <ctype.h>
#include <math.h>
#ifndef _POSIX_SOURCE
//#include <malloc.h>
#endif /* POSIX_SOURCE */
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
/* Lines added by Chuck McDevitt for WIN32 support */
#ifdef WIN32
#ifndef _POSIX_
#include <io.h>
#ifndef S_ISREG
#define S_ISREG(m) (((m)&_S_IFMT) == _S_IFREG)
#define S_ISFIFO(m) (((m)&_S_IFMT) == _S_IFIFO)
#endif
#endif
#ifndef stat
#define stat _stat
#endif
#ifndef fdopen
#define fdopen _fdopen
#endif
#ifndef open
#define open _open
#endif
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif
#ifndef O_CREAT
#define O_CREAT _O_CREAT
#endif
#endif
/* End of lines added by Chuck McDevitt for WIN32 support */
#include "dbgen/dsstypes.h"

static char alpha_num[65] = "0123456789abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ,";

#if defined(__STDC__) || defined(__cplusplus)
#define PROTO(s) s
#else
#define PROTO(s) ()
#endif

void usage();
void permute_dist(distribution *d, int needed, seed_t *seed, DBGenContext *ctx);

/*
 * generate a random string with length randomly selected in [min, max]
 * and using the characters in alphanum (currently includes a space
 * and comma)
 */
void tpch_a_rnd(int min, int max, seed_t *seed, char *dest) {
	DSS_HUGE i, len, char_int;

	RANDOM(len, min, max, seed);
	for (i = 0; i < len; i++) {
		if (i % 5 == 0)
			RANDOM(char_int, 0, MAX_LONG, seed);
		*(dest + i) = alpha_num[char_int & 077];
		char_int >>= 6;
	}
	*(dest + len) = '\0';
	return;
}

/*
 * embed a randomly selected member of distribution d in alpha-numeric
 * noise of a length rendomly selected between min and max at a random
 * position
 */
void e_str(distribution *d, int min, int max, seed_t *seed, char *dest) {
	char strtmp[MAXAGG_LEN + 1];
	DSS_HUGE loc;
	int len;

	tpch_a_rnd(min, max, seed, dest);
	pick_str(d, seed, strtmp);
	len = (int)strlen(strtmp);
	RANDOM(loc, 0, ((int)strlen(dest) - 1 - len), seed);
	memcpy(dest + loc, strtmp, sizeof(char) * len);

	return;
}

/*
 * return the string associate with the LSB of a uniformly selected
 * long in [1, max] where max is determined by the distribution
 * being queried
 */
int pick_str(distribution *s, seed_t *seed, char *target) {
	long i = 0;
	DSS_HUGE j;

	RANDOM(j, 1, s->list[s->count - 1].weight, seed);
	while (s->list[i].weight < j)
		i++;
	strcpy(target, s->list[i].text);
	return (i);
}

/*
 * unjulian (long date) -- return(date - STARTDATE)
 */
long unjulian(long date) {
	int i;
	long res = 0;

	for (i = STARTDATE / 1000; i < date / 1000; i++)
		res += 365 + LEAP(i);
	res += date % 1000 - 1;

	return (res);
}

long julian(long date) {
	long offset;
	long result;
	long yr;
	long yend;

	offset = date - STARTDATE;
	result = STARTDATE;

#ifdef WIN32
/* Disable warning about conditional expression is constant */
#pragma warning(disable : 4127)
#endif

	while (1) {
#ifdef WIN32
#pragma warning(default : 4127)
#endif
		yr = result / 1000;
		yend = yr * 1000 + 365 + LEAP(yr);
		if (result + offset > yend) /* overflow into next year */
		{
			offset -= yend - result + 1;
			result += 1000;
			continue;
		} else
			break;
	}
	return (result + offset);
}

#include "dbgen/dists_dss.h"

static char read_line_into_buffer(char *buffer, size_t bufsiz, const char **src) {
	size_t count = 0;
	while (**src && count < bufsiz - 1) {
		buffer[count++] = **src;
		if (**src == '\n') {
			(*src)++;
			break;
		}
		(*src)++;
	}
	buffer[count] = '\0';
	return **src;
}

/*
 * load a distribution from the embedded dists.dss into the target structure
 */
void read_dist(const char *name, distribution *target) {
	const char *src = dists_dss;
	char line[256], token[256], *c;
	long weight, count = 0, name_set = 0;

	while (read_line_into_buffer(line, sizeof(line), &src)) {
		if ((c = strchr(line, '\n')) != NULL)
			*c = '\0';
		if ((c = strchr(line, '#')) != NULL)
			*c = '\0';
		if (*line == '\0')
			continue;

		if (!name_set) {
			if (dsscasecmp(strtok(line, "\n\t "), "BEGIN"))
				continue;
			if (dsscasecmp(strtok(NULL, "\n\t "), name))
				continue;
			name_set = 1;
			continue;
		} else {
			if (!dssncasecmp(line, "END", 3)) {
				return;
			}
		}

		if (sscanf(line, "%[^|]|%ld", token, &weight) != 2)
			continue;

		if (!dsscasecmp(token, "count")) {
			target->count = weight;
			target->list = (set_member *)malloc((size_t)(weight * sizeof(set_member)));
			MALLOC_CHECK(target->list);
			target->max = 0;
			continue;
		}
		auto token_length = (int)strlen(token);
		target->list[count].text = (char *)malloc((size_t)(token_length + 1));
		MALLOC_CHECK(target->list[count].text);
		strcpy(target->list[count].text, token);
		target->list[count].length = token_length;
		target->max += weight;
		target->list[count].weight = target->max;

		count += 1;
	} /* while fgets() */

	if (count != target->count) {
		fprintf(stderr, "Read error on dist '%s'\n", name);
		exit(1);
	}
	return;
}

/*
 * agg_str(set, count) build an aggregated string from count unique
 * selections taken from set
 */
void agg_str(distribution *set, long count, seed_t *seed, char *dest, DBGenContext *ctx) {
	distribution *d;
	int i;

	d = set;
	*dest = '\0';

	permute_dist(d, (int)count, seed, ctx);
	for (i = 0; i < count; i++) {
		strcat(dest, DIST_MEMBER(set, ctx->permute[i]));
		strcat(dest, " ");
	}
	*(dest + (int)strlen(dest) - 1) = '\0';

	return;
}

long dssncasecmp(const char *s1, const char *s2, int n) {
	for (; n > 0; ++s1, ++s2, --n)
		if (tolower(*s1) != tolower(*s2))
			return ((tolower(*s1) < tolower(*s2)) ? -1 : 1);
		else if (*s1 == '\0')
			return (0);
	return (0);
}

long dsscasecmp(const char *s1, const char *s2) {
	for (; tolower(*s1) == tolower(*s2); ++s1, ++s2)
		if (*s1 == '\0')
			return (0);
	return ((tolower(*s1) < tolower(*s2)) ? -1 : 1);
}

#ifndef STDLIB_HAS_GETOPT
int optind = 0;
int opterr = 0;
char *optarg = NULL;

int getopt(int ac, char **av, char *opt) {
	static char *nextchar = NULL;
	char *cp;
	char hold;

	if (optarg == NULL) {
		optarg = (char *)malloc(BUFSIZ);
		MALLOC_CHECK(optarg);
	}

	if (!nextchar || *nextchar == '\0') {
		optind++;
		if (optind == ac)
			return (-1);
		nextchar = av[optind];
		if (*nextchar != '-')
			return (-1);
		nextchar += 1;
	}

	if (nextchar && *nextchar == '-') /* -- termination */
	{
		optind++;
		return (-1);
	} else /* found an option */
	{
		cp = strchr(opt, *nextchar);
		nextchar += 1;
		if (cp == NULL) /* not defined for this run */
			return ('?');
		if (*(cp + 1) == ':') /* option takes an argument */
		{
			if (*nextchar) {
				hold = *cp;
				cp = optarg;
				while (*nextchar)
					*cp++ = *nextchar++;
				*cp = '\0';
				*cp = hold;
			} else /* white space separated, use next arg */
			{
				if (++optind == ac)
					return ('?');
				strcpy(optarg, av[optind]);
			}
			nextchar = NULL;
		}
		return (*cp);
	}
}
#endif /* STDLIB_HAS_GETOPT */

char **mk_ascdate(void) {
	char **m;
	dss_time_t t;
	DSS_HUGE i;

	m = (char **)malloc((size_t)(TOTDATE * sizeof(char *)));
	MALLOC_CHECK(m);
	for (i = 0; i < TOTDATE; i++) {
		mk_time(i + 1, &t);
		m[i] = strdup(t.alpha);
	}

	return (m);
}

/*
 * set_state() -- initialize the RNG so that
 * appropriate data sets can be generated.
 * For each table that is to be generated, calculate the number of rows/child,
 * and send that to the seed generation routine in speed_seed.c. Note: assumes
 * that tables are completely independent. Returns the number of rows to be
 * generated by the named step.
 */
DSS_HUGE
set_state(int table, long sf, long procs, long step, DSS_HUGE *extra_rows, DBGenContext *ctx) {
	int i;
	DSS_HUGE rowcount, result;

	if (sf == 0 || step == 0)
		return (0);

	rowcount = ctx->tdefs[table].base;
	rowcount *= sf;
	*extra_rows = rowcount % procs;
	rowcount /= procs;
	result = rowcount;
	for (i = 0; i < step - 1; i++) {
		if (table == LINE) /* special case for shared seeds */
			ctx->tdefs[table].gen_seed(1, rowcount);
		else
			ctx->tdefs[table].gen_seed(0, rowcount);
		/* need to set seeds of child in case there's a dependency */
		/* NOTE: this assumes that the parent and child have the same base row
		 * count */
		if (ctx->tdefs[table].child != NONE)
			ctx->tdefs[ctx->tdefs[table].child].gen_seed(0, rowcount);
	}
	if (step > procs) /* moving to the end to generate updates */
		ctx->tdefs[table].gen_seed(0, *extra_rows);

	return (result);
}
