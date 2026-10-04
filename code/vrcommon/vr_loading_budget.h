/* A tracked loading frame that blocked on a busy GPU holds the next one off for its own cost, measured from its
 * return, so the loader keeps at least half the wall clock; cheap frames stay display-paced. */
#ifndef VR_LOADING_BUDGET_H
#define VR_LOADING_BUDGET_H

typedef struct {
	int lastReturnMs;
	int lastCostMs;
} vrLoadingBudget_t;

static inline void VR_LoadingBudgetSpent( vrLoadingBudget_t *b, int startMs, int returnMs ) {
	b->lastReturnMs = returnMs;
	b->lastCostMs = (int)( (unsigned)returnMs - (unsigned)startMs );
	if ( b->lastCostMs < 0 )
		b->lastCostMs = 0;
}

/* Unsigned differences so a wrapped millisecond clock never locks the pump out. */
static inline int VR_LoadingBudgetHeld( const vrLoadingBudget_t *b, int nowMs ) {
	return b->lastCostMs > 0 && (unsigned)nowMs - (unsigned)b->lastReturnMs < (unsigned)b->lastCostMs;
}
#endif
