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
#include "error_msg.h"
#include "grammar_support.h"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
static thread_local int *LN;
static thread_local char *FN;

err_msg_t Errors[MAX_ERROR + 2] = {{
                                       EFLG_NO_ARG,
                                       "",
                                   },
                                   {EFLG_STR_ARG, "File '%s' not found"},
                                   {EFLG_NO_ARG, "Line exceeds maximum leng.h"},
                                   {EFLG_STR_ARG, "Memory allocation failed %s"},
                                   {EFLG_STR_ARG, "Syntax Error: \n'%s'"},
                                   {EFLG_NO_ARG, "Invalid/Out-of-range Argument"},
                                   {EFLG_STR_ARG, "'%s' is not a unique name"},
                                   {EFLG_STR_ARG, "'%s' is not a valid name"},
                                   {EFLG_NO_ARG, "Command parse failed"},
                                   {EFLG_NO_ARG, "Invalid tag found"},
                                   {EFLG_STR_ARG, "Read failed on '%s'"},
                                   {EFLG_NO_ARG, "Too Many Templates!"},
                                   {EFLG_NO_ARG, "Each workload definition must be in its own file"},
                                   {EFLG_NO_ARG, "Query Class name must be unique within a workload definition"},
                                   {EFLG_NO_ARG, "Query Template must be unique within a query class"},
                                   {EFLG_STR_ARG | EFLG_SYSTEM, "Open failed on '%s'"},
                                   {EFLG_STR_ARG, "%s  not yet implemented"}, /* QERR_NOT_IMPLEMENTED */
                                   {EFLG_STR_ARG, "string trucated to '%s'"},
                                   {EFLG_NO_ARG, "Non-terminated string"},
                                   {EFLG_STR_ARG, "failed to write to '%s'"},
                                   {EFLG_NO_ARG, "No type vector defined for distribution"},
                                   {EFLG_NO_ARG, "No weight count defined for distribution"},
                                   {EFLG_NO_ARG, "No limits defined for pricing calculations"},
                                   {EFLG_STR_ARG, "Percentage is out of bounds in substitution '%s'"},
                                   {EFLG_STR_ARG, "Name is not a distribution or table name: '%s'"},
                                   {EFLG_NO_ARG, "Cannot evaluate expression"},
                                   {EFLG_STR_ARG, "Substitution'%s' is used before being initialized"}, /* QERR_NO_INIT
                                                                                                         */
                                   {EFLG_NO_ARG, "RANGE()/LIST()/ULIST() not supported for NORMAL "
                                                 "distributions"},
                                   {EFLG_STR_ARG, "Bad Nesting; '%s' not found"},
                                   {EFLG_STR_ARG, "Include stack overflow when opening '%s'"},
                                   {EFLG_STR_ARG, "Bad function call: '%s'"},
                                   {EFLG_STR_ARG, "Bad Hierarchy Call: '%s'"},
                                   {EFLG_NO_ARG, "Must set types and weights before defining names"},
                                   {EFLG_NO_ARG, "More than 20 arguments in definition"},
                                   {EFLG_NO_ARG, "Argument type mismat.h"},
                                   {EFLG_NO_ARG, "RANGE()/LIST()/ULIST() cannot be used in the "
                                                 "same expression"}, /* QERR_RANGE_LIST
                                                                      */
                                   {EFLG_NO_ARG, "Selected scale factor is NOT valid for result publication"},
                                   {EFLG_STR_ARG, "Parameter setting failed for '%s'"},
                                   {EFLG_STR_ARG, "Table %s is being joined without an explicit rule"},
                                   {EFLG_STR_ARG, "Table %s is not yet fully defined"},
                                   {EFLG_STR_ARG, "Table %s is a child; it is populated during the build of "
                                                  "its parent (e.g., catalog_sales builds catalog returns)"},
                                   {EFLG_NO_ARG, "Command line arguments for dbgen_version exceed 200 "
                                                 "characters; truncated"},
                                   {EFLG_NO_ARG, "A query template list must be supplied using the "
                                                 "INPUT option"},                               /* QERR_NO_QUERYLIST
                                                                                                 */
                                   {EFLG_NO_ARG, "Invalid query number found in permutation!"}, /* QERR_QUERY_RANGE
                                                                                                 */
                                   {EFLG_NO_ARG, "RANGE/LIST/ULIST expressions not valid as "
                                                 "function parameters"}, /* QERR_MODIFIED_PARAM
                                                                          */
                                   {EFLG_NO_ARG, "RANGE/LIST/ULIST truncated to available "
                                                 "values"}, /* QERR_MODIFIED_PARAM
                                                             */
                                   {EFLG_NO_ARG, "This scale factor is valid for QUALIFICATION "
                                                 "ONLY"}, /* QERR_QUALIFICATION_SCALE
                                                           */
                                   {EFLG_STR_ARG, "Generating %s requires the '-update' option"}, /* QERR_TABLE_UPDATE
                                                                                                   */
                                   {0, NULL}};

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
void ProcessErrorCode(int nErrorCode, char *szRoutineName, char *szParam, int nParam) {
	switch (nErrorCode) {
	case QERR_NO_FILE:
		ReportError(QERR_NO_FILE, szParam, 1);
		break;
	case QERR_SYNTAX:
	case QERR_RANGE_ERROR:
	case QERR_NON_UNIQUE:
	case QERR_BAD_NAME:
	case QERR_DEFINE_OVERFLOW:
	case QERR_INVALID_TAG:
	case QERR_READ_FAILED:
	case QERR_NO_MEMORY:
	case QERR_LINE_TOO_LONG:
		ReportError(nErrorCode, szRoutineName, 1);
		break;
	}
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
int ReportError(int nError, const char *msg, int bExit) {
	return ReportErrorNoLine(nError, msg, bExit);
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
int ReportErrorNoLine(int nError, const char *msg, int bExit) {
	std::string e_msg = "dsdgen error";
	if (nError < MAX_ERROR) {
		switch (Errors[-nError].flags & EFLG_ARG_MASK) {
		case EFLG_NO_ARG:
			e_msg = Errors[-nError].prompt;
			break;
		case EFLG_STR_ARG:
			e_msg = duckdb::StringUtil::Format(Errors[-nError].prompt, msg ? msg : "");
			break;
		}
	}
	if (bExit) {
		throw duckdb::InvalidInputException("dsdgen: %s", e_msg);
	}
	return (nError);
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
void SetErrorGlobals(char *szFileName, int *nLineNumber) {
	FN = szFileName;
	LN = nLineNumber;

	return;
}
