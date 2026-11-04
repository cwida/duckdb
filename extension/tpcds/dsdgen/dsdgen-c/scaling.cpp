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
#include "init.h"
#include <stdio.h>
#include <assert.h>
#include <stdio.h>
#include "config.h"
#include "porting.h"
#include "dist.h"
#include "constants.h"
#include "genrand.h"
#include "columns.h"
#include "tdefs.h"
#include "error_msg.h"
#include "r_params.h"
#include "tdefs.h"
#include "tdef_functions.h"
#include "w_inventory.h"
#include "scaling.h"
#include "tpcds.idx.h"
#include "parallel.h"
#include "scd.h"

static thread_local struct SCALING_T {
	ds_key_t kBaseRowcount;
	ds_key_t kNextInsertValue;
	int nUpdatePercentage;
	ds_key_t kDayRowcount[6];
} arRowcount[MAX_TABLE + 1];
static thread_local int arUpdateDates[6];
static thread_local int arInventoryUpdateDates[6];

static int arScaleVolume[9] = {1, 10, 100, 300, 1000, 3000, 10000, 30000, 100000};

void setUpdateScaling(int table);
int row_skip(int tbl, ds_key_t count);

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
int getScaleSlot(int nTargetGB) {
	int i;

	for (i = 0; nTargetGB > arScaleVolume[i]; i++)
		;

	return (i);
}

/*
 * Routine: LogScale(void)
 * Purpose: use the command line volume target, in GB, to calculate the global
 * rowcount multiplier Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects: arRowcounts are set to the appropriate number of rows for the
 * target scale factor
 * TODO: None
 */
static ds_key_t LogScale(int nTable, int nTargetGB) {
	int nDelta, i;
	float fOffset;
	ds_key_t hgRowcount = 0;

	i = getScaleSlot(nTargetGB);

	nDelta = dist_weight(NULL, "rowcounts", nTable + 1, i + 1) - dist_weight(NULL, "rowcounts", nTable + 1, i);
	fOffset = (float)(nTargetGB - arScaleVolume[i - 1]) / (float)(arScaleVolume[i] - arScaleVolume[i - 1]);

	hgRowcount = (int)(fOffset * (float)nDelta);
	hgRowcount += dist_weight(NULL, "rowcounts", nTable + 1, i);

	return (hgRowcount);
}

/*
 * Routine: StaticScale(void)
 * Purpose: use the command line volume target, in GB, to calculate the global
 * rowcount multiplier Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects: arRowcounts are set to the appropriate number of rows for the
 * target scale factor
 * TODO: None
 */
static ds_key_t StaticScale(int nTable, int nTargetGB) {
	return (dist_weight(NULL, "rowcounts", nTable + 1, 1));
}

/*
 * Routine: LinearScale(void)
 * Purpose: use the command line volume target, in GB, to calculate the global
 *rowcount multiplier Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions: scale factors defined in rowcounts distribution define
 *1/10/100/1000/... GB with sufficient accuracy Side Effects: arRowcounts are
 *set to the appropriate number of rows for the target scale factor
 * TODO: None
 */
static ds_key_t LinearScale(int nTable, int nTargetGB) {
	int i;
	ds_key_t hgRowcount = 0;

	for (i = 8; i >= 0; i--) /* work from large scales down)*/
	{
		/*
		 * use the defined rowcounts to build up the target GB volume
		 */
		while (nTargetGB >= arScaleVolume[i]) {
			hgRowcount += dist_weight(NULL, "rowcounts", nTable + 1, i + 1);
			nTargetGB -= arScaleVolume[i];
		}
	}

	return (hgRowcount);
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
ds_key_t getIDCount(int nTable) {
	ds_key_t kRowcount, kUniqueCount;
	tdef *pTdef;

	kRowcount = get_rowcount(nTable);
	if (nTable >= PSEUDO_TABLE_START)
		return (kRowcount);
	pTdef = getSimpleTdefsByNumber(nTable);
	if (pTdef->flags & FL_TYPE_2) {
		kUniqueCount = (kRowcount / 6) * 3;
		switch (kRowcount % 6) {
		case 1:
			kUniqueCount += 1;
			break;
		case 2:
		case 3:
			kUniqueCount += 2;
			break;
		case 4:
		case 5:
			kUniqueCount += 3;
			break;
		}
		return (kUniqueCount);
	} else {
		return (kRowcount);
	}
}

/*
 * Routine: get_rowcount(int table)
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
 * TODO: 20040820 jms Need to address special case scaling in a more general
 * fashion
 */
ds_key_t get_rowcount(int table) {

	static thread_local double nScale;
	int nTable, nMultiplier, i, nBadScale = 0, nRowcountOffset = 0;
	tdef *pTdef;

	if (!InitConstants::get_rowcount_init) {
		nScale = get_dbl("SCALE");
		if (nScale > 100000)
			ReportErrorNoLine(QERR_BAD_SCALE, NULL, 1);

		memset(arRowcount, 0, sizeof(arRowcount));
		int iScale = nScale < 1 ? 1 : int(nScale);
		for (nTable = CALL_CENTER; nTable <= MAX_TABLE; nTable++) {
			switch (iScale) {
			case 100000:
				arRowcount[nTable].kBaseRowcount = dist_weight(NULL, "rowcounts", nTable + nRowcountOffset + 1, 9);
				break;
			case 30000:
				arRowcount[nTable].kBaseRowcount = dist_weight(NULL, "rowcounts", nTable + nRowcountOffset + 1, 8);
				break;
			case 10000:
				arRowcount[nTable].kBaseRowcount = dist_weight(NULL, "rowcounts", nTable + nRowcountOffset + 1, 7);
				break;
			case 3000:
				arRowcount[nTable].kBaseRowcount = dist_weight(NULL, "rowcounts", nTable + nRowcountOffset + 1, 6);
				break;
			case 1000:
				arRowcount[nTable].kBaseRowcount = dist_weight(NULL, "rowcounts", nTable + nRowcountOffset + 1, 5);
				break;
			case 300:
				nBadScale = QERR_BAD_SCALE;
				arRowcount[nTable].kBaseRowcount = dist_weight(NULL, "rowcounts", nTable + nRowcountOffset + 1, 4);
				break;
			case 100:
				nBadScale = QERR_BAD_SCALE;
				arRowcount[nTable].kBaseRowcount = dist_weight(NULL, "rowcounts", nTable + nRowcountOffset + 1, 3);
				break;
			case 10:
				nBadScale = QERR_BAD_SCALE;
				arRowcount[nTable].kBaseRowcount = dist_weight(NULL, "rowcounts", nTable + nRowcountOffset + 1, 2);
				break;
			case 1:
				nBadScale = QERR_QUALIFICATION_SCALE;
				arRowcount[nTable].kBaseRowcount = dist_weight(NULL, "rowcounts", nTable + nRowcountOffset + 1, 1);
				break;
			default:
				nBadScale = QERR_BAD_SCALE;
				int mem = dist_member(NULL, "rowcounts", nTable + 1, 3);
				switch (mem) {
				case 2:
					arRowcount[nTable].kBaseRowcount = LinearScale(nTable + nRowcountOffset, nScale);
					break;
				case 1:
					arRowcount[nTable].kBaseRowcount = StaticScale(nTable + nRowcountOffset, nScale);
					break;
				case 3:
					arRowcount[nTable].kBaseRowcount = LogScale(nTable + nRowcountOffset, nScale);
					break;
				} /* switch(FL_SCALE_MASK) */
				break;
			} /* switch(nScale) */

			/* now adjust for the multiplier */
			nMultiplier = 1;
			if (nTable < PSEUDO_TABLE_START) {
				pTdef = getSimpleTdefsByNumber(nTable);
				nMultiplier = (pTdef->flags & FL_TYPE_2) ? 2 : 1;
			}
			for (i = 1; i <= dist_member(NULL, "rowcounts", nTable + 1, 2); i++) {
				nMultiplier *= 10;
			}
			arRowcount[nTable].kBaseRowcount *= nMultiplier;
			if (arRowcount[nTable].kBaseRowcount >= 0) {
				if (nScale < 1) {
					int mem = dist_member(NULL, "rowcounts", nTable + 1, 3);
					if (!(mem == 1 && nMultiplier == 1)) {
						arRowcount[nTable].kBaseRowcount = int(arRowcount[nTable].kBaseRowcount * nScale);
					}
					if (arRowcount[nTable].kBaseRowcount == 0) {
						arRowcount[nTable].kBaseRowcount = 1;
					}
				}
			}
		} /* for each table */

		//		if (nBadScale && !is_set("QUIET"))
		//			ReportErrorNoLine(nBadScale, NULL, 0);

		InitConstants::get_rowcount_init = 1;
	}

	if (table == INVENTORY)
		return (sc_w_inventory(nScale));
	if (table == S_INVENTORY)
		return (getIDCount(ITEM) * get_rowcount(WAREHOUSE) * 6);

	return (arRowcount[table].kBaseRowcount);
}

/*
 * Routine: setUpdateDates
 * Purpose: determine the dates for fact table updates
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
void setUpdateDates(void) {
	assert(0);
	int nDay, nUpdate, i;
	date_t dtTemp;

	nUpdate = get_int("UPDATE");
	while (nUpdate--) {
		/* pick two adjacent days in the low density zone */
		arUpdateDates[0] = getSkewedJulianDate(calendar_low, 0);
		jtodt(&dtTemp, arUpdateDates[0]);
		dist_weight(&nDay, "calendar", day_number(&dtTemp) + 1, calendar_low);
		if (nDay)
			arUpdateDates[1] = arUpdateDates[0] + 1;
		else
			arUpdateDates[1] = arUpdateDates[0] - 1;

		/*
		 * pick the related Thursdays for inventory
		 * 1. shift first date to the Thursday in the current update week
		 * 2. move forward/back to get into correct comparability zone
		 * 3. set next date to next/prior Thursday based on comparability zone
		 */
		jtodt(&dtTemp, arUpdateDates[0] + (4 - set_dow(&dtTemp)));
		dist_weight(&nDay, "calendar", day_number(&dtTemp), calendar_low);
		arInventoryUpdateDates[0] = dtTemp.julian;
		if (!nDay) {
			jtodt(&dtTemp, dtTemp.julian - 7);
			arInventoryUpdateDates[0] = dtTemp.julian;
			dist_weight(&nDay, "calendar", day_number(&dtTemp), calendar_low);
			if (!nDay)
				arInventoryUpdateDates[0] += 14;
		}

		arInventoryUpdateDates[1] = arInventoryUpdateDates[0] + 7;
		jtodt(&dtTemp, arInventoryUpdateDates[1]);
		dist_weight(&nDay, "calendar", day_number(&dtTemp) + 1, calendar_low);
		if (!nDay)
			arInventoryUpdateDates[1] -= 14;

		/* repeat for medium calendar zone */
		arUpdateDates[2] = getSkewedJulianDate(calendar_medium, 0);
		jtodt(&dtTemp, arUpdateDates[2]);
		dist_weight(&nDay, "calendar", day_number(&dtTemp) + 1, calendar_medium);
		if (nDay)
			arUpdateDates[3] = arUpdateDates[2] + 1;
		else
			arUpdateDates[3] = arUpdateDates[2] - 1;

		jtodt(&dtTemp, arUpdateDates[2] + (4 - set_dow(&dtTemp)));
		dist_weight(&nDay, "calendar", day_number(&dtTemp), calendar_medium);
		arInventoryUpdateDates[2] = dtTemp.julian;
		if (!nDay) {
			jtodt(&dtTemp, dtTemp.julian - 7);
			arInventoryUpdateDates[2] = dtTemp.julian;
			dist_weight(&nDay, "calendar", day_number(&dtTemp), calendar_medium);
			if (!nDay)
				arInventoryUpdateDates[2] += 14;
		}

		arInventoryUpdateDates[3] = arInventoryUpdateDates[2] + 7;
		jtodt(&dtTemp, arInventoryUpdateDates[3]);
		dist_weight(&nDay, "calendar", day_number(&dtTemp), calendar_medium);
		if (!nDay)
			arInventoryUpdateDates[3] -= 14;

		/* repeat for high calendar zone */
		arUpdateDates[4] = getSkewedJulianDate(calendar_high, 0);
		jtodt(&dtTemp, arUpdateDates[4]);
		dist_weight(&nDay, "calendar", day_number(&dtTemp) + 1, calendar_high);
		if (nDay)
			arUpdateDates[5] = arUpdateDates[4] + 1;
		else
			arUpdateDates[5] = arUpdateDates[4] - 1;

		jtodt(&dtTemp, arUpdateDates[4] + (4 - set_dow(&dtTemp)));
		dist_weight(&nDay, "calendar", day_number(&dtTemp), calendar_high);
		arInventoryUpdateDates[4] = dtTemp.julian;
		if (!nDay) {
			jtodt(&dtTemp, dtTemp.julian - 7);
			arInventoryUpdateDates[4] = dtTemp.julian;
			dist_weight(&nDay, "calendar", day_number(&dtTemp), calendar_high);
			if (!nDay)
				arInventoryUpdateDates[4] += 14;
		}

		arInventoryUpdateDates[5] = arInventoryUpdateDates[4] + 7;
		jtodt(&dtTemp, arInventoryUpdateDates[5]);
		dist_weight(&nDay, "calendar", day_number(&dtTemp), calendar_high);
		if (!nDay)
			arInventoryUpdateDates[5] -= 14;
	}

	//	/*
	//	 * output the update dates for this update set
	//	 */
	//	openDeleteFile(1);
	//	for (i = 0; i < 6; i += 2)
	//		print_delete(&arUpdateDates[i]);
	//
	//	/*
	//	 * inventory uses separate dates
	//	 */
	//	openDeleteFile(2);
	//	for (i = 0; i < 6; i += 2)
	//		print_delete(&arInventoryUpdateDates[i]);
	//	openDeleteFile(0);

	return;
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
int getUpdateDate(int nTable, ds_key_t kRowcount) {
	static thread_local int nIndex = 0, nLastTable = -1;

	if (nLastTable != nTable) {
		nLastTable = nTable;
		get_rowcount(nTable);
		nIndex = 0;
	}

	for (nIndex = 0; kRowcount > arRowcount[nTable].kDayRowcount[nIndex]; nIndex++)
		if (nIndex == 5)
			break;

	if (nTable == S_INVENTORY) {
		return (arInventoryUpdateDates[nIndex]);
	} else
		return (arUpdateDates[nIndex]);
}

/*
 * Routine: getUpdateID(int nTable, ds_key_t *pDest)
 * Purpose: select the primary key for an update set row
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns: 1 if the row is new, 0 if it is reusing an existing ID
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: 20040326 jms getUpdateID() this MUST be updated for 64bit -- all usages
 * use casts today
 * TODO:	20060102 jms this will need to be looked at for parallelism at some
 * point
 */
/*
int
getUpdateID(ds_key_t *pDest, int nTable, int nColumn)
{
    int bIsUpdate = 0,
        nTemp;

    if (genrand_integer(NULL, DIST_UNIFORM, 0, 99, 0, nColumn) <
arRowcount[nTable].nUpdatePercentage)
    {
        bIsUpdate = 1;
        genrand_integer(&nTemp, DIST_UNIFORM, 1, (int)getIDCount(nTable), 0,
nColumn); *pDest = (ds_key_t)nTemp;
    }
    else
    {
        *pDest = ++arRowcount[nTable].kNextInsertValue;
    }

    return(bIsUpdate);
}
*/

/*
 * Routine: getSkewedJulianDate()
 * Purpose: return a julian date based on the given skew and column
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
int getSkewedJulianDate(int nWeight, int nColumn) {
	int i;
	date_t Date;

	pick_distribution(&i, "calendar", 1, nWeight, nColumn);
	genrand_integer(&Date.year, DIST_UNIFORM, YEAR_MINIMUM, YEAR_MAXIMUM, 0, nColumn);
	dist_member(&Date.day, "calendar", i, 3);
	dist_member(&Date.month, "calendar", i, 5);
	return (dttoj(&Date));
}

/*
 * Routine: initializeOrderUpdate()
 * Purpose: skip over prior updates for the named table
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
/*
int
initializeOrderUpdates(int nParent, int nChild, int nIDColumn, int nDateColumn,
int *pnOrderNumber)
{
   int i,
      nRowcount,
      nRowsRemaining,
      nStep = 0;
   date_t Date;


        *pnOrderNumber = 0;

      for (i=0; i < (get_int("UPDATE") - 1); i++)
        {
            nRowsRemaining = (int)get_rowcount(nParent);
         while (nRowsRemaining > 0)
         {
            nStep = nStep % 3;
            nStep += 1;
            Date.julian = getSkewedJulianDate((nStep++ % 3) + 8, nDateColumn);
            nRowcount = (int)dateScaling(getTableFromColumn(nIDColumn),
Date.julian); *pnOrderNumber += nRowcount; row_skip(nParent, nRowcount);
            row_skip(nChild, LINES_PER_ORDER * nRowcount);
            nRowsRemaining -= nRowcount;
         }
        }

      return(nStep);
}
*/

/*
 * Routine: dateScaling(int nTable, ds_key_t jDate)
 * Purpose: determine the number of rows to build for a given date and fact
 * table Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
ds_key_t dateScaling(int nTable, ds_key_t jDate) {
	static thread_local dist_t *pDist;
	d_idx_t *pDistIndex;
	date_t Date;
	int nDateWeight = 1, nCalendarTotal, nDayWeight;
	ds_key_t kRowCount = -1;
	tdef *pTdef = getSimpleTdefsByNumber(nTable);

	if (!InitConstants::dateScaling_init) {
		pDistIndex = find_dist("calendar");
		pDist = pDistIndex->dist;
		if (!pDist)
			ReportError(QERR_NO_MEMORY, "dateScaling()", 1);
		InitConstants::dateScaling_init = 1;
	}

	jtodt(&Date, (int)jDate);

	switch (nTable) {
	case STORE_SALES:
	case CATALOG_SALES:
	case WEB_SALES:
		kRowCount = get_rowcount(nTable);
		nDateWeight = calendar_sales;
		break;
	case S_CATALOG_ORDER:
		kRowCount = get_rowcount(CATALOG_SALES);
		nDateWeight = calendar_sales;
		break;
	case S_PURCHASE:
		kRowCount = get_rowcount(STORE_SALES);
		nDateWeight = calendar_sales;
		break;
	case S_WEB_ORDER:
		kRowCount = get_rowcount(WEB_SALES);
		nDateWeight = calendar_sales;
		break;
	case S_INVENTORY:
	case INVENTORY:
		nDateWeight = calendar_uniform;
		kRowCount = get_rowcount(WAREHOUSE) * getIDCount(ITEM);
		break;
	default:
		ReportErrorNoLine(QERR_TABLE_NOP, pTdef->name, 1);
		break;
	}

	if (nTable != INVENTORY) /* inventory rowcount is uniform thorughout the year */
	{
		if (is_leap(Date.year))
			nDateWeight += 1;

		nCalendarTotal = dist_max(pDist, nDateWeight);
		nCalendarTotal *= 5; /* assumes date range is 5 years */

		dist_weight(&nDayWeight, "calendar", day_number(&Date), nDateWeight);
		kRowCount *= nDayWeight;
		kRowCount += nCalendarTotal / 2;
		kRowCount /= nCalendarTotal;
	}

	return (kRowCount);
}

/*
 * Routine: getUpdateBase(int nTable)
 * Purpose: return the offset to the first order in this update set for a given
 * table Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
ds_key_t getUpdateBase(int nTable) {
	return (arRowcount[nTable - S_BRAND].kNextInsertValue);
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
void setUpdateScaling(int nTable) {
	tdef *pTdef;
	int i, nBaseTable;
	ds_key_t kNewRowcount = 0;

	pTdef = getSimpleTdefsByNumber(nTable);
	if (!(pTdef->flags & FL_SOURCE_DDL) || !(pTdef->flags & FL_DATE_BASED) || (pTdef->flags & FL_NOP))
		return;

	switch (nTable) {
	case S_PURCHASE:
		nBaseTable = STORE_SALES;
		break;
	case S_CATALOG_ORDER:
		nBaseTable = CATALOG_SALES;
		break;
	case S_WEB_ORDER:
		nBaseTable = WEB_SALES;
		break;
	case S_INVENTORY:
		nBaseTable = INVENTORY;
		break;
	default:
		INTERNAL("Invalid table in setUpdateScaling");
	}

	arRowcount[nTable].kNextInsertValue = arRowcount[nTable].kBaseRowcount;

	for (i = 0; i < 6; i++) {
		kNewRowcount += dateScaling(nBaseTable, arUpdateDates[i]);
		arRowcount[nTable].kDayRowcount[i] = kNewRowcount;
	}

	arRowcount[nTable].kBaseRowcount = kNewRowcount;
	arRowcount[nTable].kNextInsertValue += kNewRowcount * (get_int("update") - 1);

	return;
}
