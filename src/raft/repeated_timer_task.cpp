// libraft - Quorum-based replication of states across machines.
// Copyright (c) 2016 Baidu.com, Inc. All Rights Reserved

// Author: Zhangyi Chen (chenzhangyi01@baidu.com)
// Date: 2016/11/02 13:53:30

#include "raft/repeated_timer_task.h"
#include "raft/util.h"

namespace raft {

RepeatedTimerTask::RepeatedTimerTask()
    : _timeout_ms(0)
    , _stopped(true)
    , _running(false)
    , _destroyed(false)
    , _invoking(false)
{}

RepeatedTimerTask::~RepeatedTimerTask()
{
    CHECK(!_running) << "Is still running";
    CHECK(_destroyed) << "destroy() must be invoked before descrution";
}

int RepeatedTimerTask::init(int timeout_ms) {
    _timeout_ms = timeout_ms;
    _destroyed = false;
    _stopped = true;
    _running = false;
    _timer = raft_timer_t();
    return 0;
}

void RepeatedTimerTask::stop() {
    BAIDU_SCOPED_LOCK(_mutex);
    RAFT_RETURN_IF(_stopped);
    _stopped = true;
    CHECK(_running);
    const int rc = raft_timer_del(_timer);
    if (rc == 0) {
        _running = false;
        return;
    }
}

void RepeatedTimerTask::on_timedout() {
    std::unique_lock<raft_mutex_t> lck(_mutex);
    _invoking = true;
    lck.unlock();
    //   ^^^NOTE: don't invoke run() inside lock to avoid the dead-lock issue
    run();
    lck.lock();
    _invoking = false;
    CHECK(_running);
    if (_stopped) {
        if (_destroyed) {
            on_destroy();
        }
        _running = false;
        return;
    }
    return schedule(lck);
}

void RepeatedTimerTask::start() {
    // Implementation considers the following conditions:
    //   - The first time start() was invoked
    //   - stop() was not invoked() 
    //   - stop() was invoked and _timer was successfully deleted
    //   - stop() was invoked but _timer was not successfully deleted:
    //       a) _timer is still running right now
    //       b) _timer is finished
    std::unique_lock<raft_mutex_t> lck(_mutex);
    RAFT_RETURN_IF(_destroyed);
    RAFT_RETURN_IF(!_stopped);
    _stopped = false;

    RAFT_RETURN_IF(_running);
                 //  ^^^ _timer was not successfully deleted and the former task
                 // is still running, in which case on_timedout would invoke
                 // schedule as it would not see _stopped
    _running = true;
    schedule(lck);
}

void* RepeatedTimerTask::run_on_timedout_in_new_thread(void* arg) {
    RepeatedTimerTask* m = (RepeatedTimerTask*)arg;
    m->on_timedout();
    return NULL;
}

void RepeatedTimerTask::on_timedout(void* arg) {
    // Start a bthread to invoke run() so we won't block the timer thread.
    // as run() might access the disk so the time it takes is probably beyond
    // expection
    bthread_t tid;
    if (bthread_start_background(
                &tid, NULL, run_on_timedout_in_new_thread, arg) != 0) {
        PLOG(ERROR) << "Fail to start bthread";
        run_on_timedout_in_new_thread(arg);
    }
}

void RepeatedTimerTask::schedule(std::unique_lock<raft_mutex_t>& lck) {
    _next_duetime = 
            base::milliseconds_from_now(adjust_timeout_ms(_timeout_ms));
    if (raft_timer_add(&_timer, _next_duetime, on_timedout, this) != 0) {
        lck.unlock();
        LOG(ERROR) << "Fail to add timer";
        return on_timedout(this);
    }
}

void RepeatedTimerTask::reset() {
    std::unique_lock<raft_mutex_t> lck(_mutex);
    RAFT_RETURN_IF(_stopped);
    CHECK(_running);
    const int rc = raft_timer_del(_timer);
    if (rc == 0) {
        return schedule(lck);
    }
    // else on_timedout would invoke schdule
}

void RepeatedTimerTask::reset(int timeout_ms) {
    std::unique_lock<raft_mutex_t> lck(_mutex);
    _timeout_ms = timeout_ms;
    RAFT_RETURN_IF(_stopped);
    CHECK(_running);
    const int rc = raft_timer_del(_timer);
    if (rc == 0) {
        return schedule(lck);
    }
    // else on_timedout would invoke schdule
}

void RepeatedTimerTask::destroy() {
    std::unique_lock<raft_mutex_t> lck(_mutex);
    RAFT_RETURN_IF(_destroyed);
    _destroyed = true;
    if (!_running) {
        CHECK(_stopped);
        lck.unlock();
        on_destroy();
        return;
    }
    RAFT_RETURN_IF(_stopped);
    _stopped = true;
    const int rc = raft_timer_del(_timer);
    if (rc == 0) {
        _running = false;
        lck.unlock();
        on_destroy();
        return;
    }
    CHECK(_running);
}

void RepeatedTimerTask::describe(std::ostream& os, bool use_html) {
    (void)use_html;
    std::unique_lock<raft_mutex_t> lck(_mutex);
    const bool stopped = _stopped;
    const bool running = _running;
    const bool destroyed = _destroyed;
    const bool invoking = _invoking;
    const timespec duetime = _next_duetime;
    const int timeout_ms = _timeout_ms;
    lck.unlock();
    os << "timeout(" << timeout_ms << "ms)";
    if (destroyed) {
        os << " DESTROYED";
    }
    if (stopped) {
        os << " STOPPED";
    }
    if (running) {
        if (invoking) {
            os << " INVOKING";
        } else {
            os << " SCHEDULING(in " 
               << base::timespec_to_milliseconds(duetime) - base::gettimeofday_ms()
               << "ms)";
        }
    }
}

}  // namespace raft
