#include "condor_common.h"
#include "condor_daemon_core.h"
#include "dc_coroutines.h"

using namespace condor;

dc::AwaitableDeadlineReaper::AwaitableDeadlineReaper() {
	reaperID = daemonCore->Register_Reaper(
		"AwaitableDeadlineReaper::reaper",
		(ReaperHandlercpp) & AwaitableDeadlineReaper::reaper,
		"AwaitableDeadlineReaper::reaper",
		this
	);
}


dc::AwaitableDeadlineReaper::~AwaitableDeadlineReaper() {
	// Do NOT destroy() the_coroutine here.  The coroutine may still
	// needs it state, because the lifetime of this object could be
	// shorter than the lifetime of the coroutine.  (For example, a
	// coroutine could declare two locals of this type, one of which
	// has a longer lifetime than the other.)

	// Cancel the reaper.  (Which holds a pointer to this.)
	if( reaperID != -1 ) {
		daemonCore->Cancel_Reaper( reaperID );
	}

	// Cancel any timers.  (Each holds a pointer to this.)
	for( auto [timerID, dummy] : timerIDToPIDMap ) {
		daemonCore->Cancel_Timer( timerID );
	}
}


bool
dc::AwaitableDeadlineReaper::born( pid_t pid, int timeout ) {
	auto [dummy, inserted] = pids.insert(pid);
	if(! inserted) { return false; }
	// dprintf( D_ZKM, "Inserted %d into %p\n", pid, & pids );

	// Register a timer for this process.
	int timerID = daemonCore->Register_Timer(
		timeout, TIMER_NEVER,
		(TimerHandlercpp) & AwaitableDeadlineReaper::timer,
		"AwaitableDeadlineReaper::timer",
		this
	);
	timerIDToPIDMap[timerID] = pid;

	return true;
}


int
dc::AwaitableDeadlineReaper::reaper( pid_t pid, int status ) {
	ASSERT(pids.contains(pid));

	// We will never hear from this process again, so forget about it.
	pids.erase(pid);

	// Make sure we don't hear from the timer.
	for( auto [a_timerID, a_pid] : timerIDToPIDMap ) {
		if( a_pid == pid ) {
			daemonCore->Cancel_Timer(a_timerID);
			timerIDToPIDMap.erase(a_timerID);
			break;
		}
	}

	the_pid = pid;
	timed_out = false;
	the_status = status;
	ASSERT(the_coroutine);
	the_coroutine.resume();

	return 0;
}


void
dc::AwaitableDeadlineReaper::timer( int timerID ) {
	ASSERT(timerIDToPIDMap.contains(timerID));
	int pid = timerIDToPIDMap[timerID];
	ASSERT(pids.contains(pid));

	// We don't remove the PID; it's up to the co_await'ing function
	// to decide what to do when the timer fires.  This does mean that
	// you'll get another event if you kill() a timed-out child, but
	// because we can safely remove the timer in the reaper, you won't
	// get a timer event after a reaper event.

	the_pid = pid;
	timed_out = true;
	the_status = -1;
	ASSERT(the_coroutine);
	the_coroutine.resume();
}
