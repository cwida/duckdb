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
#include <stdio.h>
#if !defined(_WIN32) && !defined(__MSVC32_)
#include <netinet/in.h>
#endif
#include <math.h>
#include "decimal.h"
#include "constants.h"
#include "dist.h"
#include "r_params.h"
#include "genrand.h"
#include "tdefs.h"
#include "tables.h"
#include "build_support.h"
#include "genrand.h"
#include "columns.h"
#include "StringBuffer.h"
#include "error_msg.h"
#include "scaling.h"
#include "init.h"

/*
 * Routine: hierarchy_item
 * Purpose:
 *	select the hierarchy entry for this level
 * Algorithm: Assumes a top-down ordering
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
void hierarchy_item(int h_level, ds_key_t *id, char **name, ds_key_t kIndex) {
	static thread_local int nLastCategory = -1, nLastClass = -1, nBrandBase;
	int nBrandCount;
	static thread_local char *szClassDistName = NULL;
	char sTemp[6];

	switch (h_level) {
	case I_CATEGORY:
		nLastCategory = pick_distribution(name, "categories", 1, 1, h_level);
		*id = nLastCategory;
		nBrandBase = nLastCategory;
		nLastClass = -1;
		break;
	case I_CLASS:
		if (nLastCategory == -1)
			ReportErrorNoLine(DBGEN_ERROR_HIERACHY_ORDER, "I_CLASS before I_CATEGORY", 1);
		dist_member(&szClassDistName, "categories", nLastCategory, 2);
		nLastClass = pick_distribution(name, szClassDistName, 1, 1, h_level);
		nLastCategory = -1;
		*id = nLastClass;
		break;
	case I_BRAND:
		if (nLastClass == -1)
			ReportErrorNoLine(DBGEN_ERROR_HIERACHY_ORDER, "I_BRAND before I_CLASS", 1);
		dist_member(&nBrandCount, szClassDistName, nLastClass, 2);
		*id = kIndex % nBrandCount + 1;
		mk_word(*name, "brand_syllables", nBrandBase * 10 + nLastClass, 45, I_BRAND);
		sprintf(sTemp, " #%d", (int)*id);
		strcat(*name, sTemp);
		*id += (nBrandBase * 1000 + nLastClass) * 1000;
		break;
	default:
		INTERNAL("Invalid call to hierarchy_item");
	}

	return;
}

/*
 * Routine: mk_companyname()
 * Purpose:
 *	yet another member of a set of routines used for address creation
 * Algorithm:
 *	create a hash, based on an index value, so that the same result can be
 *derived reliably and then build a word from a syllable set Data Structures:
 *
 * Params:
 *	char * dest: target for resulting name
 *	int nTable: to allow differing distributions
 *	int nCompany: index value
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO:
 *	20010615 JMS return code is meaningless
 *	20030422 JMS should be replaced if there is no per-table variation
 */
int mk_companyname(char *dest, int nTable, int nCompany) {
	mk_word(dest, "syllables", nCompany, 10, CC_COMPANY_NAME);

	return (0);
}

/*
 * Routine: set_locale()
 * Purpose:
 *	generate a reasonable lattitude and longitude based on a region and the USGS
 *data on 3500 counties in the US Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: 20011230 JMS set_locale() is just a placeholder; do we need geographic
 *coords?
 */
int set_locale(int nRegion, decimal_t *longitude, decimal_t *latitude) {
	static thread_local int init = 0;
	static thread_local decimal_t dZero;

	if (!init) {
		strtodec(&dZero, "0.00");
		init = 1;
	}

	memcpy(longitude, &dZero, sizeof(decimal_t));
	memcpy(latitude, &dZero, sizeof(decimal_t));

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
void bitmap_to_dist(void *pDest, const char *distname, ds_key_t *modulus, int vset, int stream) {
	int32_t m, s;
	char msg[80];

	if ((s = distsize(distname)) == -1) {
		sprintf(msg, "Invalid distribution name '%s'", distname);
		INTERNAL(msg);
	}
	m = (int32_t)((*modulus % s) + 1);
	*modulus /= s;

	dist_member(pDest, distname, m, vset);

	return;
}

/*
 * Routine: void dist_to_bitmap(int *pDest, char *szDistName, int nValueSet, int
 * nWeightSet, int nStream) Purpose: Reverse engineer a composite key based on
 * distributions Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
void dist_to_bitmap(int *pDest, const char *szDistName, int nValue, int nWeight, int nStream) {
	*pDest *= distsize(szDistName);
	*pDest += pick_distribution(NULL, szDistName, nValue, nWeight, nStream);

	return;
}

/*
 * Routine: void random_to_bitmap(int *pDest, int nDist, int nMin, int nMax, int
 * nMean, int nStream) Purpose: Reverse engineer a composite key based on an
 * integer range Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
void random_to_bitmap(int *pDest, int nDist, int nMin, int nMax, int nMean, int nStream) {
	*pDest *= nMax;
	*pDest += genrand_integer(NULL, nDist, nMin, nMax, nMean, nStream);

	return;
}

/*
 * Routine: mk_word()
 * Purpose:
 *	generate a gibberish word from a given syllable set
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
void mk_word(char *dest, const char *syl_set, ds_key_t src, int char_cnt, int col) {
	ds_key_t i = src, nSyllableCount;
	char *cp;

	*dest = '\0';
	while (i > 0) {
		nSyllableCount = distsize(syl_set);
		dist_member(&cp, syl_set, (int)(i % nSyllableCount) + 1, 1);
		i /= nSyllableCount;
		if ((int)(strlen(dest) + strlen(cp)) <= char_cnt)
			strcat(dest, cp);
		else
			break;
	}

	return;
}

/*
 * Routine: mk_surrogate()
 * Purpose: create a character based surrogate key from a 64-bit value
 * Algorithm: since the RNG routines produce a 32bit value, and surrogate keys
 *can reach beyond that, use the RNG output to generate the lower end of a
 *random string, and build the upper end from a ds_key_t Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls: ltoc()
 * Assumptions: output is a 16 character string. Space is not checked
 * Side Effects:
 * TODO:
 * 20020830 jms may need to define a 64-bit form of htonl() for portable shift
 *operations
 */
static char szXlate[16] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P'};
static void ltoc(char *szDest, unsigned long nVal) {
	int i;
	char c;

	for (i = 0; i < 8; i++) {
		c = szXlate[(nVal & 0xF)];
		*szDest++ = c;
		nVal >>= 4;
	}
	*szDest = '\0';
}

void mk_bkey(char *szDest, ds_key_t kPrimary, int nStream) {
	unsigned long nTemp;

	nTemp = (unsigned long)(kPrimary >> 32);
	ltoc(szDest, nTemp);

	nTemp = (unsigned long)(kPrimary & 0xFFFFFFFF);
	ltoc(szDest + 8, nTemp);

	return;
}

/*
 * Routine: embed_string(char *szDest, char *szDist, int nValue, int nWeight,
 * int nStream) Purpose: Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int embed_string(char *szDest, const char *szDist, int nValue, int nWeight, int nStream) {
	int nPosition;
	char *szWord = NULL;

	pick_distribution(&szWord, szDist, nValue, nWeight, nStream);
	nPosition = genrand_integer(NULL, DIST_UNIFORM, 0, strlen(szDest) - strlen(szWord) - 1, 0, nStream);
	memcpy(&szDest[nPosition], szWord, sizeof(char) * strlen(szWord));

	return (0);
}

/*
 * Routine: set_scale()
 * Purpose: link SCALE and SCALE_INDEX
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
int SetScaleIndex(const char *szName, const char *szValue) {
	int nScale;
	char szScale[2];

	if ((nScale = atoi(szValue)) == 0)
		nScale = 1;

	nScale = 1 + (int)log10(nScale);
	szScale[0] = '0' + nScale;
	szScale[1] = '\0';

	set_int("_SCALE_INDEX", szScale);

	return (atoi(szValue));
}

/*
 * Routine: adjust the valid date window for source schema tables, based on
 *	based on the update count, update window size, etc.
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
void setUpdateDateRange(int nTable, date_t *pMinDate, date_t *pMaxDate) {
	static thread_local int nUpdateNumber;

	if (!InitConstants::setUpdateDateRange_init) {
		nUpdateNumber = get_int("UPDATE");
		InitConstants::setUpdateDateRange_init = 1;
	}

	switch (nTable) /* no per-table changes at the moment; but could be */
	{
	default:
		strtodt(pMinDate, WAREHOUSE_LOAD_DATE);
		pMinDate->julian += UPDATE_INTERVAL * (nUpdateNumber - 1);
		jtodt(pMinDate, pMinDate->julian);
		jtodt(pMaxDate, pMinDate->julian + UPDATE_INTERVAL);
		break;
	}

	return;
}
