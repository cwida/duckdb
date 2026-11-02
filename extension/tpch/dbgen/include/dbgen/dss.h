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
 * general definitions and control information for the DSS code
 * generator; if it controls the data set, it's here
 */
#ifndef DSS_H
#define DSS_H

#include "dbgen/config.h"

#ifdef TPCH
#define NAME "TPC-H"
#endif
#ifdef TPCR
#define NAME "TPC-R"
#endif
#ifndef NAME
#error Benchmark version must be defined in config.h
#endif
#define TPC     "Transaction Processing Performance Council"
#define C_DATES "1994 - 2010"

#include "dbgen/config.h"
#include "dbgen/shared.h"

#include <stdio.h>
#include <stdlib.h>

// some defines to avoid r warnings
#define exit(status)
#define printf(...)
#define fprintf(...)

#define NONE       -1
#define PART       0
#define PSUPP      1
#define SUPP       2
#define CUST       3
#define ORDER      4
#define LINE       5
#define ORDER_LINE 6
#define PART_PSUPP 7
#define NATION     8
#define REGION     9
#define UPDATE     10
#define MAX_TABLE  11
#define ONE_STREAM 1
#define ADD_AT_END 2

#ifdef MAX
#undef MAX
#endif
#ifdef MIN
#undef MIN
#endif
#define MAX(a, b) ((a > b) ? a : b)
#define MIN(A, B) ((A) < (B) ? (A) : (B))

#define INTERNAL_ERROR(p) // {fprintf(stderr,"%s", p);abort();}
#define LN_CNT            4
// static char lnoise[4] = {'|', '/', '-', '\\' };
#define LIFENOISE(n, var) // if (verbose > 0) fprintf(stderr, "%c\b", lnoise[(var%LN_CNT)])

#define MALLOC_CHECK(var)                                                                                              \
	if ((var) == NULL) {                                                                                               \
		fprintf(stderr, "Malloc failed at %s:%d\n", __FILE__, __LINE__);                                               \
		exit(1);                                                                                                       \
	}
#define OPEN_CHECK(var, path)                                                                                          \
	if ((var) == NULL) {                                                                                               \
		fprintf(stderr, "Open failed for %s at %s:%d\n", path, __FILE__, __LINE__);                                    \
		exit(1);                                                                                                       \
	}
#ifndef MAX_CHILDREN
#define MAX_CHILDREN 1000
#endif

/*
 * macros that control sparse keys
 *
 * refer to Porting.Notes for a complete explanation
 */
#ifndef BITS_PER_LONG
#define BITS_PER_LONG 32
#define MAX_LONG      0x7FFFFFFF
#endif /* BITS_PER_LONG */
#define SPARSE_BITS         2
#define SPARSE_KEEP         3
#define MK_SPARSE(key, seq) (((((key >> 3) << 2) | (seq & 0x0003)) << 3) | (key & 0x0007))

#define RANDOM(tgt, lower, upper, seed)   dss_random(&tgt, lower, upper, seed)
#define RANDOM64(tgt, lower, upper, seed) dss_random64(&tgt, lower, upper, seed)

typedef struct {
	long weight;
	char *text;
	/* byte length cached when loading dists.dss */
	int length;
} set_member;

typedef struct {
	int count;
	int max;
	set_member *list;
} distribution;
/*
 * some handy access functions
 */
#define DIST_SIZE(d)       d->count
#define DIST_MEMBER(d, i)  ((set_member *)((d)->list + i))->text

typedef struct {
	const char *name;
	const char *comment;
	DSS_HUGE base;
	int (*loader)();
	long (*gen_seed)(int, int);
	int child;
	DSS_HUGE vtotal;
} tdef;

typedef struct SEED_T {
	long table;
	DSS_HUGE value;
	DSS_HUGE usage;
	DSS_HUGE boundary;
#ifdef RNG_TEST
	DSS_HUGE nCalls;
#endif
} seed_t;

#define PROTO(s) s

struct DBGenContext;

/* bm_utils.c */
void tpch_a_rnd PROTO((int min, int max, seed_t *seed, char *dest));
int tx_rnd PROTO((long min, long max, long column, char *tgt));
long julian PROTO((long date));
long unjulian PROTO((long date));
long dssncasecmp PROTO((const char *s1, const char *s2, int n));
long dsscasecmp PROTO((const char *s1, const char *s2));
int pick_str PROTO((distribution * s, seed_t *seed, char *target));
void agg_str PROTO((distribution * set, long count, seed_t *seed, char *dest, DBGenContext *ctx));
void read_dist PROTO((const char *name, distribution *target));
void embed_str PROTO((distribution * d, int min, int max, int stream, char *dest));
void permute_dist PROTO((distribution * d, int needed, seed_t *seed, DBGenContext *ctx));
#ifndef STDLIB_HAS_GETOPT
int getopt PROTO((int arg_cnt, char **arg_vect, char *oprions));
#endif /* STDLIB_HAS_GETOPT */
DSS_HUGE set_state PROTO((int t, long scale, long procs, long step, DSS_HUGE *e, DBGenContext *ctx));

/* rnd.c */
DSS_HUGE NextRand PROTO((DSS_HUGE nSeed));
DSS_HUGE UnifInt PROTO((DSS_HUGE nLow, DSS_HUGE nHigh, seed_t *seed));
void dss_random(DSS_HUGE *tgt, DSS_HUGE min, DSS_HUGE max, seed_t *seed);
void row_start(int t, DBGenContext *ctx);
void row_stop_h(int t, DBGenContext *ctx);
void dump_seeds_ds(int t, seed_t *seeds);

/* text.c */
#define MAX_GRAMMAR_LEN 12  /* max length of grammar component */
#define MAX_SENT_LEN    256 /* max length of populated sentence */
#define TEXT_POOL_SIZE  (300 * 1024 * 1024) /* size of the pseudo-text pool */
#define RNG_PER_SENT    27  /* max number of RNG calls per sentence */

void init_text_pool PROTO((long bSize, DBGenContext *ctx));
void free_text_pool PROTO(());

void dbg_text PROTO((char *t, int min, int max, seed_t *seed));
int dbg_text_source PROTO((int min, int max, seed_t *seed, const char **source));

#ifdef DECLARER
#define EXTERN
#else
#define EXTERN extern
#endif /* DECLARER */

EXTERN distribution nations;
EXTERN distribution nations2;
EXTERN distribution regions;
EXTERN distribution o_priority_set;
EXTERN distribution l_instruct_set;
EXTERN distribution l_smode_set;
EXTERN distribution l_category_set;
EXTERN distribution l_rflag_set;
EXTERN distribution c_mseg_set;
EXTERN distribution colors;
EXTERN distribution p_types_set;
EXTERN distribution p_cntr_set;

/* distributions that control text generation */
EXTERN distribution articles;
EXTERN distribution nouns;
EXTERN distribution adjectives;
EXTERN distribution adverbs;
EXTERN distribution prepositions;
EXTERN distribution verbs;
EXTERN distribution terminators;
EXTERN distribution auxillaries;
EXTERN distribution np;
EXTERN distribution vp;
EXTERN distribution grammar;

EXTERN int refresh;
EXTERN int resume;
EXTERN long verbose;
EXTERN long force;
EXTERN long updates;
EXTERN long table;
EXTERN int set_seeds;
EXTERN char *d_path;

/* added for segmented updates */
EXTERN int insert_segments;
EXTERN int delete_segments;
EXTERN int insert_orders_segment;
EXTERN int insert_lineitem_segment;
EXTERN int delete_segment;

/*****************************************************************
 ** table level defines use the following naming convention: t_ccc_xxx
 ** with: t, a table identifier
 **       ccc, a column identifier
 **       xxx, a limit type
 ****************************************************************
 */

/*
 * defines which control the parts table
 */
#define P_SIZE     126
#define P_NAME_SCL 5
#define P_MFG_TAG  "Manufacturer#"
#define P_MFG_FMT  "%%s%%0%d%s"
#define P_MFG_MIN  1
#define P_MFG_MAX  5
#define P_BRND_TAG "Brand#"
#define P_BRND_FMT "%%s%%0%d%s"
#define P_BRND_MIN 1
#define P_BRND_MAX 5
#define P_SIZE_MIN 1
#define P_SIZE_MAX 50
#define P_MCST_MIN 100
#define P_MCST_MAX 99900
#define P_MCST_SCL 100.0
#define P_RCST_MIN 90000
#define P_RCST_MAX 200000
#define P_RCST_SCL 100.0
/*
 * defines which control the suppliers table
 */
#define S_SIZE        145
#define S_NAME_TAG    "Supplier#"
#define S_NAME_FMT    "%%s%%0%d%s"
#define S_ABAL_MIN    -99999
#define S_ABAL_MAX    999999
#define S_CMNT_MAX    101
#define S_CMNT_BBB    10 /* number of BBB comments/SF */
#define BBB_DEADBEATS 50 /* % that are complaints */
#define BBB_BASE      "Customer "
#define BBB_COMPLAIN  "Complaints"
#define BBB_COMMEND   "Recommends"
#define BBB_CMNT_LEN  19
#define BBB_BASE_LEN  9
#define BBB_TYPE_LEN  10

/*
 * defines which control the partsupp table
 */
#define PS_SIZE     145
#define PS_SKEY_MIN 0
#define PS_SKEY_MAX ((ctx->tdefs[SUPP].base - 1) * ctx->scale_factor)
#define PS_SCST_MIN 100
#define PS_SCST_MAX 100000
#define PS_QTY_MIN  1
#define PS_QTY_MAX  9999
/*
 * defines which control the customers table
 */
#define C_SIZE     165
#define C_NAME_TAG "Customer#"
#define C_NAME_FMT "%%s%%0%d%s"
#define C_MSEG_MAX 5
#define C_ABAL_MIN -99999
#define C_ABAL_MAX 999999
/*
 * defines which control the order table
 */
#define O_SIZE      109
#define O_CKEY_MIN  1
#define O_CKEY_MAX  (ctx->tdefs[CUST].base * ctx->scale_factor)
#define O_ODATE_MIN STARTDATE
#define O_ODATE_MAX (STARTDATE + TOTDATE - (L_SDTE_MAX + L_RDTE_MAX) - 1)
#define O_CLRK_TAG  "Clerk#"
#define O_CLRK_FMT  "%%s%%0%d%s"
#define O_CLRK_SCL  1000
#define O_LCNT_MIN  1
#define O_LCNT_MAX  7

/*
 * defines which control the lineitem table
 */
#define L_SIZE     144L
#define L_QTY_MIN  1
#define L_QTY_MAX  50
#define L_TAX_MIN  0
#define L_TAX_MAX  8
#define L_DCNT_MIN 0
#define L_DCNT_MAX 10
#define L_PKEY_MIN 1
#define L_PKEY_MAX (ctx->tdefs[PART].base * ctx->scale_factor)
#define L_SDTE_MIN 1
#define L_SDTE_MAX 121
#define L_CDTE_MIN 30
#define L_CDTE_MAX 90
#define L_RDTE_MIN 1
#define L_RDTE_MAX 30
/*
 * defines which control the time table
 */
#define T_SIZE      30
#define T_START_DAY 3 /* wednesday ? */
#define LEAP(y)     ((!(y % 4) && (y % 100)) ? 1 : 0)

/*******************************************************************
 *******************************************************************
 ***
 *** general or inter table defines
 ***
 *******************************************************************
 *******************************************************************/
#define SUPP_PER_PART   4
#define ORDERS_PER_CUST 10 /* sync this with CUST_MORTALITY */
#define CUST_MORTALITY  3  /* portion with have no orders */
#define NATIONS_MAX     90 /* limited by country codes in phone numbers */
#define PHONE_FMT       "%02d-%03d-%03d-%04d"
#define STARTDATE       92001
#define CURRENTDATE     95168
#define ENDDATE         98365
#define TOTDATE         2557
#define UPD_PCT         10
#define MAX_STREAM      47
#define V_STR_LOW       0.4
#define PENNIES         100 /* for scaled int money arithmetic */
#define Q11_FRACTION    (double)0.0001
/*
 * max and min SF in GB; Larger SF will require changes to the build routines
 */
#define MIN_SCALE 1.0
#define MAX_SCALE 100000.0
/*
 * beyond this point we need to allow for BCD calculations
 */
#define MAX_32B_SCALE           1000.0
#define LONG2HUGE(src, dst)     *dst = (DSS_HUGE)src
#define HUGE2LONG(src, dst)     *dst = (long)src
#define HUGE_SET(src, dst)      *dst = *src
#define HUGE_MUL(op1, op2)      *op1 *= op2
#define HUGE_DIV(op1, op2)      *op1 /= op2
#define HUGE_ADD(op1, op2, dst) *dst = *op1 + op2
#define HUGE_SUB(op1, op2, dst) *dst = *op1 - op2
#define HUGE_MOD(op1, op2)      *op1 % op2
#define HUGE_CMP(op1, op2)      (*op1 == *op2) ? 0 : (*op1 < *op2) - 1 : 1

/******** environmental variables and defaults ***************/
#define PATH_TAG    "DSS_PATH"   /* environment var to override ... */
#define PATH_DFLT   "."          /* default directory to hold tables */
#define CONFIG_TAG  "DSS_CONFIG" /* environment var to override ... */
#define CONFIG_DFLT "."          /* default directory to config files */
#define ADHOC_TAG   "DSS_ADHOC"  /* environment var to override ... */
#define ADHOC_DFLT  "adhoc.dss"  /* default file name for adhoc vars */

/******* output macros ********/
#ifndef SEPARATOR
#define SEPARATOR '|' /* field spearator for generated flat files */
#endif
/* Data type flags for a single print routine */
#define DT_STR 0
#ifndef MVS
#define DT_VSTR DT_STR
#else
#define DT_VSTR 1
#endif /* MVS */
#define DT_INT   2
#define DT_HUGE  3
#define DT_KEY   4
#define DT_MONEY 5
#define DT_CHR   6

int dbg_print(int dt, FILE *tgt, void *data, int len, int eol);
#define PR_STR(f, str, len)       dbg_print(DT_STR, f, (void *)str, len, 1)
#define PR_VSTR(f, str, len)      dbg_print(DT_VSTR, f, (void *)str, len, 1)
#define PR_VSTR_LAST(f, str, len) dbg_print(DT_VSTR, f, (void *)str, len, 0)
#define PR_INT(f, str)            dbg_print(DT_INT, f, (void *)str, 0, 1)
#define PR_HUGE(f, str)           dbg_print(DT_HUGE, f, (void *)str, 0, 1)
#define PR_HUGE_LAST(f, str)      dbg_print(DT_HUGE, f, (void *)str, 0, 0)
#define PR_KEY(f, str)            dbg_print(DT_KEY, f, (void *)str, 0, -1)
#define PR_MONEY(f, str)          dbg_print(DT_MONEY, f, (void *)str, 0, 1)
#define PR_CHR(f, str)            dbg_print(DT_CHR, f, (void *)str, 0, 1)
#define PR_STRT(fp)                                 /* any line prep for a record goes here */
#define PR_END(fp)                fprintf(fp, "\n") /* finish the record here */
#ifdef MDY_DATE
#define PR_DATE(tgt, yr, mn, dy) snprintf(tgt, 11, "%02d-%02d-19%02d", mn, dy, yr)
#else
#define PR_DATE(tgt, yr, mn, dy) snprintf(tgt, 11, "19%02ld-%02ld-%02ld", yr, mn, dy)
#endif /* DATE_FORMAT */

/*
 * verification macros
 */
#define VRF_STR(t, d)                                                                                                  \
	{                                                                                                                  \
		char *xx = d;                                                                                                  \
		while (*xx)                                                                                                    \
			ctx->tdefs[t].vtotal += *xx++;                                                                             \
	}
#define VRF_INT(t, d)  ctx->tdefs[t].vtotal += d
#define VRF_HUGE(t, d) ctx->tdefs[t].vtotal = *((long *)&d) + *((long *)(&d + 1))
/* assume float is a 64 bit quantity */
#define VRF_MONEY(t, d) ctx->tdefs[t].vtotal = *((long *)&d) + *((long *)(&d + 1))
#define VRF_CHR(t, d)   ctx->tdefs[t].vtotal += d
#define VRF_STRT(t)
#define VRF_END(t)

/*********** distribuitons currently defined *************/
#define UNIFORM 0

/*
 * seed indexes; used to separate the generation of individual columns
 */
#define P_MFG_SD      0
#define P_BRND_SD     1
#define P_TYPE_SD     2
#define P_SIZE_SD     3
#define P_CNTR_SD     4
#define P_RCST_SD     5
#define PS_QTY_SD     7
#define PS_SCST_SD    8
#define O_SUPP_SD     10
#define O_CLRK_SD     11
#define O_ODATE_SD    13
#define L_QTY_SD      14
#define L_DCNT_SD     15
#define L_TAX_SD      16
#define L_SHIP_SD     17
#define L_SMODE_SD    18
#define L_PKEY_SD     19
#define L_SKEY_SD     20
#define L_SDTE_SD     21
#define L_CDTE_SD     22
#define L_RDTE_SD     23
#define L_RFLG_SD     24
#define C_NTRG_SD     27
#define C_PHNE_SD     28
#define C_ABAL_SD     29
#define C_MSEG_SD     30
#define S_NTRG_SD     33
#define S_PHNE_SD     34
#define S_ABAL_SD     35
#define P_NAME_SD     37
#define O_PRIO_SD     38
#define HVAR_SD       39
#define O_CKEY_SD     40
#define N_CMNT_SD     41
#define R_CMNT_SD     42
#define O_LCNT_SD     43
#define BBB_JNK_SD    44
#define BBB_TYPE_SD   45
#define BBB_CMNT_SD   46
#define BBB_OFFSET_SD 47

struct DBGenContext {
	~DBGenContext() {
		if (permute) {
			free(permute);
			permute = NULL;
		}
	}

	seed_t Seed[MAX_STREAM + 1] = {
	    {PART, 1, 0, 1},                           /* P_MFG_SD     0 */
	    {PART, 46831694, 0, 1},                    /* P_BRND_SD    1 */
	    {PART, 1841581359, 0, 1},                  /* P_TYPE_SD    2 */
	    {PART, 1193163244, 0, 1},                  /* P_SIZE_SD    3 */
	    {PART, 727633698, 0, 1},                   /* P_CNTR_SD    4 */
	    {NONE, 933588178, 0, 1},                   /* text pregeneration  5 */
	    {PART, 804159733, 0, 2},                   /* P_CMNT_SD    6 */
	    {PSUPP, 1671059989, 0, SUPP_PER_PART},     /* PS_QTY_SD    7 */
	    {PSUPP, 1051288424, 0, SUPP_PER_PART},     /* PS_SCST_SD   8 */
	    {PSUPP, 1961692154, 0, SUPP_PER_PART * 2}, /* PS_CMNT_SD   9 */
	    {ORDER, 1227283347, 0, 1},                 /* O_SUPP_SD    10 */
	    {ORDER, 1171034773, 0, 1},                 /* O_CLRK_SD    11 */
	    {ORDER, 276090261, 0, 2},                  /* O_CMNT_SD    12 */
	    {ORDER, 1066728069, 0, 1},                 /* O_ODATE_SD   13 */
	    {LINE, 209208115, 0, O_LCNT_MAX},          /* L_QTY_SD     14 */
	    {LINE, 554590007, 0, O_LCNT_MAX},          /* L_DCNT_SD    15 */
	    {LINE, 721958466, 0, O_LCNT_MAX},          /* L_TAX_SD     16 */
	    {LINE, 1371272478, 0, O_LCNT_MAX},         /* L_SHIP_SD    17 */
	    {LINE, 675466456, 0, O_LCNT_MAX},          /* L_SMODE_SD   18 */
	    {LINE, 1808217256, 0, O_LCNT_MAX},         /* L_PKEY_SD    19 */
	    {LINE, 2095021727, 0, O_LCNT_MAX},         /* L_SKEY_SD    20 */
	    {LINE, 1769349045, 0, O_LCNT_MAX},         /* L_SDTE_SD    21 */
	    {LINE, 904914315, 0, O_LCNT_MAX},          /* L_CDTE_SD    22 */
	    {LINE, 373135028, 0, O_LCNT_MAX},          /* L_RDTE_SD    23 */
	    {LINE, 717419739, 0, O_LCNT_MAX},          /* L_RFLG_SD    24 */
	    {LINE, 1095462486, 0, O_LCNT_MAX * 2},     /* L_CMNT_SD    25 */
	    {CUST, 881155353, 0, 9},                   /* C_ADDR_SD    26 */
	    {CUST, 1489529863, 0, 1},                  /* C_NTRG_SD    27 */
	    {CUST, 1521138112, 0, 3},                  /* C_PHNE_SD    28 */
	    {CUST, 298370230, 0, 1},                   /* C_ABAL_SD    29 */
	    {CUST, 1140279430, 0, 1},                  /* C_MSEG_SD    30 */
	    {CUST, 1335826707, 0, 2},                  /* C_CMNT_SD    31 */
	    {SUPP, 706178559, 0, 9},                   /* S_ADDR_SD    32 */
	    {SUPP, 110356601, 0, 1},                   /* S_NTRG_SD    33 */
	    {SUPP, 884434366, 0, 3},                   /* S_PHNE_SD    34 */
	    {SUPP, 962338209, 0, 1},                   /* S_ABAL_SD    35 */
	    {SUPP, 1341315363, 0, 2},                  /* S_CMNT_SD    36 */
	    {PART, 709314158, 0, 92},                  /* P_NAME_SD    37 */
	    {ORDER, 591449447, 0, 1},                  /* O_PRIO_SD    38 */
	    {LINE, 431918286, 0, 1},                   /* HVAR_SD      39 */
	    {ORDER, 851767375, 0, 1},                  /* O_CKEY_SD    40 */
	    {NATION, 606179079, 0, 2},                 /* N_CMNT_SD    41 */
	    {REGION, 1500869201, 0, 2},                /* R_CMNT_SD    42 */
	    {ORDER, 1434868289, 0, 1},                 /* O_LCNT_SD    43 */
	    {SUPP, 263032577, 0, 1},                   /* BBB offset   44 */
	    {SUPP, 753643799, 0, 1},                   /* BBB type     45 */
	    {SUPP, 202794285, 0, 1},                   /* BBB comment  46 */
	    {SUPP, 715851524, 0, 1}                    /* BBB junk     47 */
	};

	static constexpr double dM = 2147483647.0;

	tdef tdefs[10] = {
	    {"part.tbl", "part table", 200000, NULL, NULL, PSUPP, 0},
	    {"partsupp.tbl", "partsupplier table", 200000, NULL, NULL, NONE, 0},
	    {"supplier.tbl", "suppliers table", 10000, NULL, NULL, NONE, 0},
	    {"customer.tbl", "customers table", 150000, NULL, NULL, NONE, 0},
	    {"orders.tbl", "order table", 150000, NULL, NULL, LINE, 0},
	    {"lineitem.tbl", "lineitem table", 150000, NULL, NULL, NONE, 0},
	    {"orders.tbl", "orders/lineitem tables", 150000, NULL, NULL, LINE, 0},
	    {"part.tbl", "part/partsupplier tables", 200000, NULL, NULL, PSUPP, 0},
	    {"nation.tbl", "nation table", NATIONS_MAX, NULL, NULL, NONE, 0},
	    {"region.tbl", "region table", NATIONS_MAX, NULL, NULL, NONE, 0},
	};

	long scale_factor = 1;
	long *permute = NULL;
};

#endif /* DSS_H */
