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
#include "porting.h"
#include "tpcds_idx.hpp"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>

#ifdef _WIN32
#include <io.h>
#include <search.h>
#include <stdlib.h>
#include <winsock.h>
#else
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif
#ifdef NCR
#include <sys/types.h>
#endif
#ifndef USE_STDLIB_H
#include <malloc.h>
#endif
#include "config.h"
#include "date.h"
#include "dcomp.h"
#include "decimal.h"
#include "dist.h"
#include "error_msg.h"
#include "genrand.h"
#include "r_params.h"
#ifdef TEST
option_t options[] = {{"DISTRIBUTIONS", OPT_STR, 2, "read distributions from file <s>", NULL, "tester_dist.idx"}, NULL};

char params[2];
struct {
	char *name;
} tdefs[] = {NULL};
#endif

/* NOTE: these need to be in sync with a_dist.h */
#define D_NAME_LEN 20
#define FL_LOADED  0x01
static int load_dist(d_idx_t *d);

struct DistCache {
	~DistCache() {
		Clear();
	}

	void Clear() {
		if (!idx) {
			entry_count = 0;
			return;
		}
		for (int i = 0; i < entry_count; i++) {
			FreeDistribution(idx[i]);
		}
		free(idx);
		idx = NULL;
		entry_count = 0;
	}

	static void FreeDistribution(d_idx_t &entry) {
		auto dist = entry.dist;
		if (!dist) {
			return;
		}
		if (dist->weight_sets) {
			for (int i = 0; i < entry.w_width; i++) {
				free(dist->weight_sets[i]);
			}
		}
		if (dist->value_sets) {
			for (int i = 0; i < entry.v_width; i++) {
				free(dist->value_sets[i]);
			}
		}
		free(dist->type_vector);
		free(dist->weight_sets);
		free(dist->maximums);
		free(dist->value_sets);
		free(dist->names);
		free(dist->strings);
		free(dist);
		entry.dist = NULL;
		entry.flags = 0;
	}

	int entry_count = 0;
	d_idx_t *idx = NULL;
};

/*
 * Routine: di_compare()
 * Purpose: comparison routine for two d_idx_t entries; used by qsort
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
int di_compare(const void *op1, const void *op2) {
	d_idx_t *ie1 = (d_idx_t *)op1, *ie2 = (d_idx_t *)op2;

	return (strcasecmp(ie1->name, ie2->name));
}

/*
 * Routine: find_dist(char *name)
 * Purpose: translate from dist_t name to d_idx_t *
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
d_idx_t *find_dist(const char *name) {
	static thread_local DistCache dist_cache;
	auto &entry_count = dist_cache.entry_count;
	auto &idx = dist_cache.idx;
	d_idx_t key, *id = NULL;
	int i;
	FILE *ifp;
	int32_t temp;

	/* load the index if this is the first time through */
	if (!InitConstants::find_dist_init || !idx) {
		dist_cache.Clear();

		/* open the dist file */
		auto read_ptr = tpcds_idx;
		//			if ((ifp = fopen(get_str("DISTRIBUTIONS"), "rb")) == NULL) {
		//				fprintf(stderr, "Error: open of distributions failed: ");
		//				perror(get_str("DISTRIBUTIONS"));
		//				exit(1);
		//			}
		memcpy(&temp, read_ptr, sizeof(int32_t));
		read_ptr += sizeof(int32_t);
		//			if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
		//				fprintf(stderr, "Error: read of index count failed: ");
		//				perror(get_str("DISTRIBUTIONS"));
		//				exit(2);
		//			}
		entry_count = ntohl(temp);
		read_ptr = tpcds_idx + tpcds_idx_len - (entry_count * IDX_SIZE);
		//			if ((temp = fseek(ifp, -entry_count * IDX_SIZE, SEEK_END)) < 0) {
		//				fprintf(stderr, "Error: lseek to index failed: ");
		//				fprintf(stderr, "attempting to reach %d\nSystem error: ", (int)(-entry_count * IDX_SIZE));
		//				perror(get_str("DISTRIBUTIONS"));
		//				exit(3);
		//			}
		idx = (d_idx_t *)malloc(entry_count * sizeof(d_idx_t));
		MALLOC_CHECK(idx);
		for (i = 0; i < entry_count; i++) {
			memset(idx + i, 0, sizeof(d_idx_t));
			//				if (fread(idx[i].name, 1, D_NAME_LEN, ifp) < D_NAME_LEN) {
			//					fprintf(stderr, "Error: read index failed (1): ");
			//					perror(get_str("DISTRIBUTIONS"));
			//					exit(2);
			//				}
			memcpy(idx[i].name, read_ptr, D_NAME_LEN);
			read_ptr += D_NAME_LEN;
			idx[i].name[D_NAME_LEN] = '\0';
			memcpy(&temp, read_ptr, sizeof(int32_t));
			read_ptr += sizeof(int32_t);
			//				if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
			//					fprintf(stderr, "Error: read index failed (2): ");
			//					perror(get_str("DISTRIBUTIONS"));
			//					exit(2);
			//				}
			idx[i].index = ntohl(temp);
			memcpy(&temp, read_ptr, sizeof(int32_t));
			read_ptr += sizeof(int32_t);
			//				if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
			//					fprintf(stderr, "Error: read index failed (4): ");
			//					perror(get_str("DISTRIBUTIONS"));
			//					exit(2);
			//				}
			idx[i].offset = ntohl(temp);
			memcpy(&temp, read_ptr, sizeof(int32_t));
			read_ptr += sizeof(int32_t);
			//				if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
			//					fprintf(stderr, "Error: read index failed (5): ");
			//					perror(get_str("DISTRIBUTIONS"));
			//					exit(2);
			//				}
			idx[i].str_space = ntohl(temp);
			memcpy(&temp, read_ptr, sizeof(int32_t));
			read_ptr += sizeof(int32_t);
			//				if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
			//					fprintf(stderr, "Error: read index failed (6): ");
			//					perror(get_str("DISTRIBUTIONS"));
			//					exit(2);
			//				}
			idx[i].length = ntohl(temp);
			memcpy(&temp, read_ptr, sizeof(int32_t));
			read_ptr += sizeof(int32_t);
			//				if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
			//					fprintf(stderr, "Error: read index failed (7): ");
			//					perror(get_str("DISTRIBUTIONS"));
			//					exit(2);
			//				}
			idx[i].w_width = ntohl(temp);
			memcpy(&temp, read_ptr, sizeof(int32_t));
			read_ptr += sizeof(int32_t);
			//				if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
			//					fprintf(stderr, "Error: read index failed (8): ");
			//					perror(get_str("DISTRIBUTIONS"));
			//					exit(2);
			//				}
			idx[i].v_width = ntohl(temp);
			memcpy(&temp, read_ptr, sizeof(int32_t));
			read_ptr += sizeof(int32_t);
			//				if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
			//					fprintf(stderr, "Error: read index failed (9): ");
			//					perror(get_str("DISTRIBUTIONS"));
			//					exit(2);
			//				}
			idx[i].name_space = ntohl(temp);
			idx[i].dist = NULL;
		}
		qsort((void *)idx, entry_count, sizeof(d_idx_t), di_compare);
		InitConstants::find_dist_init = 1;

		/* make sure that this is read one thread at a time */
		//			fclose(ifp);
	}

	/* find the distribution, if it exists and move to it */
	strcpy(key.name, name);
	id = (d_idx_t *)bsearch((void *)&key, (void *)idx, entry_count, sizeof(d_idx_t), di_compare);
	if (id != NULL)                 /* found a valid distribution */
		if (id->flags != FL_LOADED) /* but it needs to be loaded */
			load_dist(id);

	return (id);
}

/*
 * Routine: load_dist(int fd, dist_t *d)
 * Purpose: load a particular distribution
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
static int load_dist(d_idx_t *di) {
	int res = 0, i, j;
	dist_t *d;
	int32_t temp;
	FILE *ifp;

	if (di->flags != FL_LOADED) /* make sure no one beat us to it */
	{
		auto read_ptr = tpcds_idx;
		//		if ((ifp = fopen(get_str("DISTRIBUTIONS"), "rb")) == NULL) {
		//			fprintf(stderr, "Error: open of distributions failed: ");
		//			perror(get_str("DISTRIBUTIONS"));
		//			exit(1);
		//		}
		read_ptr += di->offset;
		//		if ((temp = fseek(ifp, di->offset, SEEK_SET)) < 0) {
		//			fprintf(stderr, "Error: lseek to distribution failed: ");
		//			perror("load_dist()");
		//			exit(2);
		//		}

		di->dist = (dist_t *)malloc(sizeof(struct DIST_T));
		MALLOC_CHECK(di->dist);
		memset(di->dist, 0, sizeof(struct DIST_T));
		d = di->dist;

		// fprintf(stderr, "\ndist %s ", di->name);

		/* load the type information */
		d->type_vector = (int *)malloc(sizeof(int32_t) * di->v_width);
		MALLOC_CHECK(d->type_vector);
		for (i = 0; i < di->v_width; i++) {
			memcpy(&temp, read_ptr, sizeof(int32_t));
			read_ptr += sizeof(int32_t);
			//			if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
			//				fprintf(stderr, "Error: read of type vector failed for '%s': ", di->name);
			//				perror("load_dist()");
			//				exit(3);
			//			}
			d->type_vector[i] = ntohl(temp);
			// fprintf(stderr, "type %d, ", d->type_vector[i]);
		}

		/* load the weights */
		d->weight_sets = (int **)malloc(sizeof(int *) * di->w_width);
		d->maximums = (int *)malloc(sizeof(int32_t) * di->w_width);
		MALLOC_CHECK(d->weight_sets);
		MALLOC_CHECK(d->maximums);
		for (i = 0; i < di->w_width; i++) {
			*(d->weight_sets + i) = (int *)malloc(di->length * sizeof(int32_t));
			MALLOC_CHECK(*(d->weight_sets + i));
			d->maximums[i] = 0;
			for (j = 0; j < di->length; j++) {
				memcpy(&temp, read_ptr, sizeof(int32_t));
				read_ptr += sizeof(int32_t);
				//				if (fread(&temp, 1, sizeof(int32_t), ifp) < 0) {
				//					fprintf(stderr, "Error: read of weights failed: ");
				//					perror("load_dist()");
				//					exit(3);
				//				}
				*(*(d->weight_sets + i) + j) = ntohl(temp);
				/* calculate the maximum weight and convert sets to cummulative
				 */
				d->maximums[i] += d->weight_sets[i][j];
				d->weight_sets[i][j] = d->maximums[i];
			}
		}

		/* load the value offsets */
		d->value_sets = (int **)malloc(sizeof(int *) * di->v_width);
		MALLOC_CHECK(d->value_sets);
		for (i = 0; i < di->v_width; i++) {
			*(d->value_sets + i) = (int *)malloc(di->length * sizeof(int32_t));
			MALLOC_CHECK(*(d->value_sets + i));
			for (j = 0; j < di->length; j++) {
				memcpy(&temp, read_ptr, sizeof(int32_t));
				read_ptr += sizeof(int32_t);
				//				if (fread(&temp, 1, sizeof(int32_t), ifp) != sizeof(int32_t)) {
				//					fprintf(stderr, "Error: read of values failed: ");
				//					perror("load_dist()");
				//					exit(4);
				//				}
				*(*(d->value_sets + i) + j) = ntohl(temp);
			}
		}

		/* load the column aliases, if they were defined */
		if (di->name_space) {
			d->names = (char *)malloc(di->name_space);
			MALLOC_CHECK(d->names);
			memcpy(d->names, read_ptr, di->name_space * sizeof(char));
			read_ptr += di->name_space * sizeof(char);
			//			if (fread(d->names, 1, di->name_space * sizeof(char), ifp) < 0) {
			//				fprintf(stderr, "Error: read of names failed: ");
			//				perror("load_dist()");
			//				exit(599);
			//			}
		}

		/* and finally the values themselves */
		d->strings = (char *)malloc(sizeof(char) * di->str_space);
		MALLOC_CHECK(d->strings);
		memcpy(d->strings, read_ptr, di->str_space * sizeof(char));
		read_ptr += di->str_space * sizeof(char);
		//		if (fread(d->strings, 1, di->str_space * sizeof(char), ifp) < 0) {
		//			fprintf(stderr, "Error: read of strings failed: ");
		//			perror("load_dist()");
		//			exit(5);
		//		}

		//		fclose(ifp);

		//
		//	fprintf(stderr, "%s {\n", di->name);
		//
		//	// type_vector
		//	fprintf(stderr, "{");
		//
		//	for (int i = 0 ; i < di->v_width; i++) {
		//		fprintf(stderr, "%d", d->type_vector[i]);
		//
		//		if (i < di->v_width-1) {
		//			fprintf(stderr, ", ");
		//
		//		}
		//	}
		//	fprintf(stderr, "},");
		//
		//// weight_sets
		//	fprintf(stderr, "{");
		//		for (int i = 0 ; i < di->w_width; i++) {
		//			fprintf(stderr, "{");
		//
		//					for (int j = 0 ; j < di->length; j++) {
		//
		//						fprintf(stderr, "%d", d->weight_sets[i][j]);
		//
		//
		//
		//						if (j < di->length-1) {
		//							fprintf(stderr, ", ");
		//
		//						}
		//					}
		//					fprintf(stderr, "},");
		//
		//
		//
		//			if (i < di->w_width-1) {
		//				fprintf(stderr, ", ");
		//
		//			}
		//		}
		//		fprintf(stderr, "},");
		//
		//
		//	// maximums
		//			fprintf(stderr, "{");
		//
		//		for (int i = 0 ; i < di->w_width; i++) {
		//			fprintf(stderr, "%d", d->maximums[i]);
		//
		//			if (i < di->w_width-1) {
		//				fprintf(stderr, ", ");
		//
		//			}
		//		}
		//		fprintf(stderr, "},");
		//
		//
		//
		//		// value sets
		//		fprintf(stderr, "{");
		//			for (int i = 0 ; i < di->v_width; i++) {
		//				fprintf(stderr, "{");
		//
		//						for (int j = 0 ; j < di->length; j++) {
		//
		//							fprintf(stderr, "%d", d->value_sets[i][j]);
		//
		//
		//
		//							if (j < di->length-1) {
		//								fprintf(stderr, ", ");
		//
		//							}
		//						}
		//						fprintf(stderr, "},");
		//
		//
		//
		//				if (i < di->v_width-1) {
		//					fprintf(stderr, ", ");
		//
		//				}
		//			}
		//			fprintf(stderr, "},");
		//
		//
		//// strings
		//
		//
		//			fprintf(stderr, "{");
		//
		//				for (int i = 0 ; i < di->str_space; i++) {
		//					fprintf(stderr, "%d", (int) d->strings[i]);
		//
		//					if (i < di->str_space-1) {
		//						fprintf(stderr, ", ");
		//
		//					}
		//				}
		//				fprintf(stderr, "},");
		//
		//
		//
		//			// names
		//
		//			fprintf(stderr, "{");
		//
		//				for (int i = 0 ; i < di->name_space; i++) {
		//					fprintf(stderr, "%d", (int) d->names[i]);
		//
		//					if (i < di->name_space-1) {
		//						fprintf(stderr, ", ");
		//
		//					}
		//				}
		//				fprintf(stderr, "},");
		//
		//
		//
		//			// size
		//	fprintf(stderr, "%d}\n", d->size);

		di->flags = FL_LOADED;
	}

	return (res);
}

/*
 * Routine: void *dist_op()
 * Purpose: select a value/weight from a distribution
 * Algorithm:
 * Data Structures:
 *
 * Params:	char *d_name
 *			int vset: which set of values
 *			int wset: which set of weights
 * Returns: appropriate data type cast as a void *
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: 20000317 Need to be sure this is portable to NT and others
 */
int dist_op(void *dest, int op, const char *d_name, int vset, int wset, int stream) {
	d_idx_t *d;
	dist_t *dist;
	int level, index = 0, dt;
	char *char_val;
	int i_res = 1;

	if ((d = find_dist(d_name)) == NULL) {
		char msg[80];
		sprintf(msg, "Invalid distribution name '%s'", d_name);
		INTERNAL(msg);
		assert(d != NULL);
	}

	dist = d->dist;

	if (op == 0) {
		genrand_integer(&level, DIST_UNIFORM, 1, dist->maximums[wset - 1], 0, stream);
		while (level > dist->weight_sets[wset - 1][index] && index < d->length)
			index += 1;
		dt = vset - 1;
		if ((index >= d->length) || (dt > d->v_width))
			INTERNAL("Distribution overrun");
		char_val = dist->strings + dist->value_sets[dt][index];
	} else {
		index = vset - 1;
		dt = wset - 1;
		if (index >= d->length || index < 0) {
			INTERNAL("Distribution over-run/under-run");
		}
		char_val = dist->strings + dist->value_sets[dt][index];
	}

	switch (dist->type_vector[dt]) {
	case TKN_VARCHAR:
		if (dest)
			*(char **)dest = (char *)char_val;
		break;
	case TKN_INT:
		i_res = atoi(char_val);
		if (dest)
			*(int *)dest = i_res;
		break;
	case TKN_DATE:
		if (dest == NULL) {
			dest = (date_t *)malloc(sizeof(date_t));
			MALLOC_CHECK(dest);
		}
		strtodt(*(date_t **)dest, char_val);
		break;
	case TKN_DECIMAL:
		if (dest == NULL) {
			dest = (decimal_t *)malloc(sizeof(decimal_t));
			MALLOC_CHECK(dest);
		}
		strtodec(*(decimal_t **)dest, char_val);
		break;
	}

	return ((dest == NULL) ? i_res : index + 1); /* shift back to the 1-based indexing scheme */
}

/*
 * Routine: int dist_weight
 * Purpose: return the weight of a particular member of a distribution
 * Algorithm:
 * Data Structures:
 *
 * Params:	distribution *d
 *			int index: which "row"
 *			int wset: which set of weights
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO:
 *	20000405 need to add error checking
 */
int dist_weight(int *dest, const char *d, int index, int wset) {
	d_idx_t *d_idx;
	dist_t *dist;
	int res;

	if ((d_idx = find_dist(d)) == NULL) {
		char msg[80];
		sprintf(msg, "Invalid distribution name '%s'", d);
		INTERNAL(msg);
	}

	dist = d_idx->dist;
	assert(index > 0);
	assert(wset > 0);
	res = dist->weight_sets[wset - 1][index - 1];
	/* reverse the accumulation of weights */
	if (index > 1)
		res -= dist->weight_sets[wset - 1][index - 2];

	if (dest == NULL)
		return (res);

	*dest = res;

	return (0);
}

/*
 * Routine: int DistNameIndex()
 * Purpose: return the index of a column alias
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO:
 */
int DistNameIndex(const char *szDist, int nNameType, const char *szName) {
	d_idx_t *d_idx;
	dist_t *dist;
	int res;
	char *cp = NULL;

	if ((d_idx = find_dist(szDist)) == NULL)
		return (-1);
	dist = d_idx->dist;

	if (dist->names == NULL)
		return (-1);

	res = 0;
	cp = dist->names;
	do {
		if (strcasecmp(szName, cp) == 0)
			break;
		cp += strlen(cp) + 1;
		res += 1;
	} while (res < (d_idx->v_width + d_idx->w_width));

	if (res >= 0) {
		if ((nNameType == VALUE_NAME) && (res < d_idx->v_width))
			return (res + 1);
		if ((nNameType == WEIGHT_NAME) && (res > d_idx->v_width))
			return (res - d_idx->v_width + 1);
	}

	return (-1);
}

/*
 * Routine: int distsize(char *name)
 * Purpose: return the size of a distribution
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO:
 *	20000405 need to add error checking
 */
int distsize(const char *name) {
	d_idx_t *dist;

	dist = find_dist(name);

	if (dist == NULL)
		return (-1);

	return (dist->length);
}

/*
 * Routine: int IntegrateDist(char *szDistName, int nPct, int nStartIndex, int
 *nWeightSet) Purpose: return the index of the entry which, starting from
 *nStartIndex, would create a range comprising nPct of the total contained in
 *nWeightSet NOTE: the value can "wrap" -- that is, the returned value can be
 *less than nStartIndex Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO:
 */

int IntegrateDist(const char *szDistName, int nPct, int nStartIndex, int nWeightSet) {
	d_idx_t *pDistIndex;
	int nGoal, nSize;

	if ((nPct <= 0) || (nPct >= 100))
		return (QERR_RANGE_ERROR);

	pDistIndex = find_dist(szDistName);
	if (pDistIndex == NULL)
		return (QERR_BAD_NAME);

	if (nStartIndex > pDistIndex->length)
		return (QERR_RANGE_ERROR);

	nGoal = pDistIndex->dist->maximums[nWeightSet];
	nGoal = nGoal * nPct / 100;
	nSize = distsize(szDistName);

	while (nGoal >= 0) {
		nStartIndex++;
		nGoal -= dist_weight(NULL, szDistName, nStartIndex % nSize, nWeightSet);
	}

	return (nStartIndex);
}

/*
 * Routine: int dist_type(char *name, int nValueSet)
 * Purpose: return the type of the n-th value set in a distribution
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO:
 */
int dist_type(const char *name, int nValueSet) {
	d_idx_t *dist;

	dist = find_dist(name);

	if (dist == NULL)
		return (-1);

	if (nValueSet < 1 || nValueSet > dist->v_width)
		return (-1);

	return (dist->dist->type_vector[nValueSet - 1]);
}

/*
 * Routine: dist_active(char *szName, int nWeightSet)
 * Purpose: return number of entries with non-zero weght values
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
int dist_active(const char *szName, int nWeightSet) {
	int nSize, nResult = 0, i;

	nSize = distsize(szName);
	for (i = 1; i <= nSize; i++) {
		if (dist_weight(NULL, szName, i, nWeightSet) != 0)
			nResult += 1;
	}

	return (nResult);
}

/*
 * Routine: DistSizeToShiftWidth(char *szDist)
 * Purpose: Determine the number of bits required to select a member of the
 * distribution Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int DistSizeToShiftWidth(const char *szDist, int nWeightSet) {
	int nBits = 1, nTotal = 2, nMax;
	d_idx_t *d;

	d = find_dist(szDist);
	nMax = dist_max(d->dist, nWeightSet);

	while (nTotal < nMax) {
		nBits += 1;
		nTotal <<= 1;
	}

	return (nBits);
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
int MatchDistWeight(void *dest, const char *szDist, int nWeight, int nWeightSet, int ValueSet) {
	d_idx_t *d;
	dist_t *dist;
	int index = 0, dt, i_res, nRetcode;
	char *char_val;

	if ((d = find_dist(szDist)) == NULL) {
		char msg[80];
		sprintf(msg, "Invalid distribution name '%s'", szDist);
		INTERNAL(msg);
	}

	dist = d->dist;
	nWeight %= dist->maximums[nWeightSet - 1];

	while (nWeight > dist->weight_sets[nWeightSet - 1][index] && index < d->length)
		index += 1;
	dt = ValueSet - 1;
	if (index >= d->length)
		index = d->length - 1;
	char_val = dist->strings + dist->value_sets[dt][index];

	switch (dist->type_vector[dt]) {
	case TKN_VARCHAR:
		if (dest)
			*(char **)dest = (char *)char_val;
		break;
	case TKN_INT:
		i_res = atoi(char_val);
		if (dest)
			*(int *)dest = i_res;
		break;
	case TKN_DATE:
		if (dest == NULL) {
			dest = (date_t *)malloc(sizeof(date_t));
			MALLOC_CHECK(dest);
		}
		strtodt(*(date_t **)dest, char_val);
		break;
	case TKN_DECIMAL:
		if (dest == NULL) {
			dest = (decimal_t *)malloc(sizeof(decimal_t));
			MALLOC_CHECK(dest);
		}
		strtodec(*(decimal_t **)dest, char_val);
		break;
	}

	nRetcode = 1;
	index = 1;
	while (index < dist->maximums[nWeightSet - 1]) {
		nRetcode += 1;
		index *= 2;
	}

	return (nRetcode);
}

/*
 * Routine: findDistValue(char *szValue, char *szDistName, int nValueSet)
 * Purpose: Return the row number where the entry is found
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO:
 * 20031024 jms this routine needs to handle all data types, not just varchar
 */
int findDistValue(const char *szValue, const char *szDistName, int ValueSet) {
	int nRetValue = 1, nDistMax;
	char szDistValue[128];

	nDistMax = distsize(szDistName);

	for (nRetValue = 1; nRetValue < nDistMax; nRetValue++) {
		dist_member(&szDistValue, szDistName, nRetValue, ValueSet);
		if (strcmp(szValue, szDistValue) == 0)
			break;
	}

	if (nRetValue <= nDistMax)
		return (nRetValue);
	return (-1);
}

#ifdef TEST
main() {
	int i_res;
	char *c_res;
	decimal_t dec_res;

	init_params();

	dist_member(&i_res, "test_dist", 1, 1);
	if (i_res != 10) {
		printf("dist_member(\"test_dist\", 1, 1): %d != 10\n", i_res);
		exit(1);
	}
	dist_member(&i_res, "test_dist", 1, 2);
	if (i_res != 60) {
		printf("dist_member(\"test_dist\", 1, 2): %d != 60\n", i_res);
		exit(1);
	}
	dist_member((void *)&c_res, "test_dist", 1, 3);
	if (strcmp(c_res, "El Camino")) {
		printf("dist_member(\"test_dist\", 1, 3): %s != El Camino\n", c_res);
		exit(1);
	}
	dist_member((void *)&dec_res, "test_dist", 1, 4);
	if (strcmp(dec_res.number, "1") || strcmp(dec_res.fraction, "23")) {
		printf("dist_member(\"test_dist\", 1, 4): %s.%s != 1.23\n", dec_res.number, dec_res.fraction);
		exit(1);
	}
	dist_weight(&i_res, "test_dist", 2, 2);
	if (3 != i_res) {
		printf("dist_weight(\"test_dist\", 2, 2): %d != 3\n", i_res);
		exit(1);
	}
}
#endif /* TEST */
