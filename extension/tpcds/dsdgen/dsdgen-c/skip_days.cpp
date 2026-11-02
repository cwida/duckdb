#include "skip_days.h"
#include "date.h"
#include "constants.h"
#include "scaling.h"
#include "parallel.h"

ds_key_t skipDays(int nTable, ds_key_t *pRemainder) {
	static thread_local date_t BaseDate;
	ds_key_t jDate;
	ds_key_t kRowCount, kFirstRow, kDayCount, index = 1;

	if (!InitConstants::skipDays_init) {
		strtodt(&BaseDate, DATA_START_DATE);
		InitConstants::skipDays_init = 1;
		*pRemainder = 0;
	}

	// set initial conditions
	jDate = BaseDate.julian;
	*pRemainder = dateScaling(nTable, jDate) + index;

	// now check to see if we need to move to the
	// the next piece of a parallel build
	// move forward one day at a time
	split_work(nTable, &kFirstRow, &kRowCount);
	while (index < kFirstRow) {
		kDayCount = dateScaling(nTable, jDate);
		index += kDayCount;
		jDate += 1;
		*pRemainder = index;
	}
	if (jDate > BaseDate.julian && index >= kFirstRow) {
		jDate -= 1;
	}
	return (jDate);
}
