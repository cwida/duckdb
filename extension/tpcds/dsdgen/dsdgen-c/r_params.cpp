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
/*
 * parameter handling functions
 */
#include <stdlib.h>
#include <stdio.h>
#include "config.h"
#include "porting.h"
#include "r_params.h"
#include "tdefs.h"

#define PARAM_MAX_LEN 80

extern thread_local option_t options[];
extern thread_local char *params[];


struct ParamCache {
	~ParamCache() {
		for (int i = 0; options[i].name != NULL; i++) {
			auto index = options[i].index;
			free(params[index]);
			params[index] = NULL;
		}
	}
};

int fnd_param(const char *name);

/*
 * Routine:  load_params()
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
 * TODO:
 * 20010621 JMS shared memory not yet implemented
 */
void load_params() {
	/*
	    int i=0;
	    while (options[i].name != NULL)
	    {
	        load_param(i, GetSharedMemoryParam(options[i].index));
	        i++;
	    }
	    SetSharedMemoryStat(STAT_ROWCOUNT, get_int("STEP"), 0);
	*/
	return;
}

/*
 * Routine:  set_flag(int f)
 * Purpose:  set a toggle parameter
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
void set_flg(const char *flag) {
	int nParam;

	init_params();
	nParam = fnd_param(flag);
	if (nParam >= 0)
		strcpy(params[options[nParam].index], "Y");

	return;
}

/*
 * Routine: clr_flg(f)
 * Purpose: clear a toggle parameter
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
void clr_flg(const char *flag) {
	int nParam;

	init_params();
	nParam = fnd_param(flag);
	if (nParam >= 0)
		strcpy(params[options[nParam].index], "N");
	return;
}

/*
 * Routine: is_set(int f)
 * Purpose: return the state of a toggle parameter, or whether or not a string
 * or int parameter has been set Algorithm: Data Structures:
 *
 * Params:
 * Returns:
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int is_set(const char *flag) {
	int nParam, bIsSet = 0;

	init_params();
	nParam = fnd_param(flag);
	if (nParam >= 0) {
		if ((options[nParam].flags & TYPE_MASK) == OPT_FLG)
			bIsSet = (params[options[nParam].index][0] == 'Y') ? 1 : 0;
		else
			bIsSet = (options[nParam].flags & OPT_SET) || (strlen(options[nParam].dflt) > 0);
	}

	return (bIsSet); /* better a false negative than a false positive ? */
}

/*
 * Routine: set_int(int var, char *value)
 * Purpose: set an integer parameter
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
void set_int(const char *var, const char *val) {
	int nParam;

	init_params();
	nParam = fnd_param(var);
	if (nParam >= 0) {
		strcpy(params[options[nParam].index], val);
		options[nParam].flags |= OPT_SET;
	}
	return;
}

/*
 * Routine: get_int(char *var)
 * Purpose: return the value of an integer parameter
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
int get_int(const char *var) {
	int nParam;

	init_params();
	nParam = fnd_param(var);
	if (nParam >= 0)
		return (atoi(params[options[nParam].index]));
	else
		return (0);
}

double get_dbl(const char *var) {
	int nParam;

	init_params();
	nParam = fnd_param(var);
	if (nParam >= 0)
		return (atof(params[options[nParam].index]));
	else
		return (0);
}

/*
 * Routine: set_str(int var, char *value)
 * Purpose: set a character parameter
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
void set_str(const char *var, const char *val) {
	int nParam;

	init_params();
	nParam = fnd_param(var);
	if (nParam >= 0) {
		strcpy(params[options[nParam].index], val);
		options[nParam].flags |= OPT_SET;
	}

	return;
}

/*
 * Routine: get_str(char * var)
 * Purpose: return the value of a character parameter
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
char *get_str(const char *var) {
	int nParam;

	init_params();
	nParam = fnd_param(var);
	if (nParam >= 0)
		return (params[options[nParam].index]);
	else
		return (NULL);
}

/*
 * Routine: init_params(void)
 * Purpose: initialize a parameter set, setting default values
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
int init_params(void) {
	static thread_local ParamCache param_cache;
	int i;
	(void)param_cache;

	if (InitConstants::init_params_init)
		return (0);

	for (i = 0; options[i].name != NULL; i++) {
		auto index = options[i].index;
		if (!params[index]) {
			params[index] = (char *)malloc(PARAM_MAX_LEN * sizeof(char));
			MALLOC_CHECK(params[index]);
		}
		strncpy(params[index], options[i].dflt, PARAM_MAX_LEN - 1);
		params[index][PARAM_MAX_LEN - 1] = '\0';
		options[i].flags &= ~(OPT_SET | OPT_DFLT);
		if (*options[i].dflt)
			options[i].flags |= OPT_DFLT;
	}

	InitConstants::init_params_init = 1;

	return (0);
}

/*
 * Routine: fnd_param(char *name, int *type, char *value)
 * Purpose: traverse the defined parameters, looking for a match
 * Algorithm:
 * Data Structures:
 *
 * Params:
 * Returns: index of option
 * Called By:
 * Calls:
 * Assumptions:
 * Side Effects:
 * TODO: None
 */
int fnd_param(const char *name) {
	int i, res = -1;

	for (i = 0; options[i].name != NULL; i++) {
		if (strncasecmp(name, options[i].name, strlen(name)) == 0) {
			if (res == -1)
				res = i;
			else
				return (-1);
		}
	}

	return (res);
}

/*
 * Routine:  GetParamName(int nParam)
 * Purpose:  Translate between a parameter index and its name
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
char *GetParamName(int nParam) {
	init_params();

	return (char *)(options[nParam].name);
}

/*
 * Routine:  GetParamValue(int nParam)
 * Purpose:  Retrieve a parameters string value based on an index
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
char *GetParamValue(int nParam) {
	init_params();

	return (params[options[nParam].index]);
}

/*
 * Routine:  load_param(char *szValue, int nParam)
 * Purpose:  Set a parameter based on an index
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
int load_param(int nParam, const char *szValue) {
	init_params();

	if (options[nParam].flags & OPT_SET) /* already set from the command line */
		return (0);
	else
		strcpy(params[options[nParam].index], szValue);

	return (0);
}

/*
 * Routine:  IsIntParam(char *szValue, int nParam)
 * Purpose:  Boolean test for integer parameter
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
int IsIntParam(const char *szParam) {
	int nParam;

	if ((nParam = fnd_param(szParam)) == -1)
		return (nParam);

	return ((options[nParam].flags & OPT_INT) ? 1 : 0);
}

/*
 * Routine:  IsStrParam(char *szValue, int nParam)
 * Purpose:  Boolean test for string parameter
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
int IsStrParam(const char *szParam) {
	int nParam;

	if ((nParam = fnd_param(szParam)) == -1)
		return (nParam);

	return ((options[nParam].flags & OPT_STR) ? 1 : 0);
}

