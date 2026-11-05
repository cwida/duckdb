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
#ifndef R_DIST_H
#define R_DIST_H

#define D_NAME_LEN 20

typedef struct DIST_T {
	int *type_vector;
	int **weight_sets;
	int *maximums;
	int **value_sets;
	char *strings;
	char *names;
	int size;
} dist_t;

typedef struct D_IDX_T {
	char name[D_NAME_LEN + 1];
	int index;
	int nAllocatedLength;
	int nRemainingStrSpace;
	int offset;
	int str_space;
	int name_space;
	int length;
	int w_width;
	int v_width;
	int flags;
	dist_t *dist;
} d_idx_t;

typedef struct DISTINDEX_T {
	int nDistCount;
	int nAllocatedCount;
	d_idx_t *pEntries;
} distindex_t;

/* must match WriteDist() in dcomp.c */
#define IDX_SIZE (D_NAME_LEN + 7 * sizeof(int))

int dist_op(void *dest, int op, const char *d_name, int vset, int wset, int stream);
#define pick_distribution(dest, dist, v, w, s) dist_op(dest, 0, dist, v, w, s)
#define dist_member(dest, dist, v, w)          dist_op(dest, 1, dist, v, w, 0)
#define dist_max(dist, w)                      dist->maximums[w - 1]
int dist_weight(int *dest, const char *d, int index, int wset);
int distsize(const char *szDistname);
int dist_type(const char *szDistName, int vset);
d_idx_t *find_dist(const char *name);
int IntegrateDist(const char *szDistName, int nPct, int nStartIndex, int nWeightSet);
int dist_active(const char *szName, int nWeightSet);
int DistNameIndex(const char *szDist, int nNameType, const char *szName);
int DistSizeToShiftWidth(const char *szDist, int nWeightSet);
int MatchDistWeight(void *dest, const char *szDist, int nWeight, int nWeightSet, int ValueSet);
int findDistValue(const char *szValue, const char *szDistName, int ValueSet);
int di_compare(const void *op1, const void *op2);

#define DIST_UNIFORM     0x0001
#define DIST_EXPONENTIAL 0x0002
/* sales and returns are special; they must match calendar.dst */
#define DIST_SALES     3
#define DIST_RETURNS   5
#define DIST_CHAR      0x0004
#define DIST_INT       0x0008
#define DIST_NAMES_SET 0xff00

/* DistNameIndex needs to know what sort of name we are trying to match */
#define VALUE_NAME  0x0000
#define WEIGHT_NAME 0x0001

#endif /* R_DIST_H */
