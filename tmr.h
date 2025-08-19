//
//Timer
//
#ifndef _TMR_H
#define _TMR_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C"{
#endif

typedef struct
{
	uint32_t tStart;
	uint32_t tTimeout;
	uint32_t tDiff;
	uint32_t running;
} tick_timer_t;

/* start a timer (timeout units=seconds) */
extern void UtilTickTimerStart(tick_timer_t *t, uint32_t timeout_secs);

/* start a timer (timeout units=milliseconds) */
extern void UtilTickTimerStartMs(tick_timer_t *t, uint32_t timeout_ms);

// Run tick timer
// returns 0=timer not exired; 1=timer expired
extern int UtilTickTimerRun(tick_timer_t *t);

extern void UtilStopTimer(tick_timer_t *t);

#if defined(__cplusplus)
}
#endif

#endif // _TMR_H

