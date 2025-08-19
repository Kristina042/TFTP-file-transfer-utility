//
//Timer
//

#include "tmr.h"
#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
	#include <windows.h>
#else
	#include<time.h>
#endif

//get num of seconds since boot

/* start a timer (timeout units=seconds) */
uint32_t get_tick_count()
{
	#ifdef _WIN32
		return (uint32_t)GetTickCount();
	#else
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);

		// Convert seconds and nanoseconds to milliseconds
		uint64_t milliseconds = (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

		// Cast to uint32_t to mimic GetTickCount behavior (wrap around at ~49.7 days)
		return (uint32_t)milliseconds;
	#endif
}

void UtilTickTimerStart(tick_timer_t *t, uint32_t timeout_secs)
{
	// get number of seconds since boot
	t->tStart = get_tick_count();
	t->tTimeout = (timeout_secs * 1000);
	t->tDiff = 0;
	t->running = 1;
}

/* start a timer (timeout units=milliseconds) */
void UtilTickTimerStartMs(tick_timer_t *t, uint32_t timeout_ms)
{
	// get number of seconds since boot
	t->tStart = get_tick_count();
	t->tTimeout = timeout_ms;
	t->tDiff = 0;
	t->running = 1;
}

// Run tick timer
// returns 0=timer not exired; 1=timer expired
int UtilTickTimerRun(tick_timer_t *t)
{
	uint32_t now, diff;

	if (!t->running)
		return 0;

	now = get_tick_count();

	diff = (now - t->tStart);
	t->tDiff = diff;

	if (diff >= t->tTimeout)
	{
		t->running = 0;
		
		return 1;
	}

	return 0;
}

void UtilStopTimer(tick_timer_t *t)
{
	t->running = 0;
}
