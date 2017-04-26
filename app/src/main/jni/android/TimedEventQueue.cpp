/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <errno.h>
#include "Common.h"

#define LOG_TAG "TimedEventQueue"
#include "Log.h"

#include "TimedEventQueue.h"


namespace openamedia {

TimedEventQueue::TimedEventQueue()
    : mNextEventID(1),
      mRunning(false),
      mStopped(false) {
}

TimedEventQueue::~TimedEventQueue() {
    stop();
}

void TimedEventQueue::start() {
    if (mRunning) {
        return;
    }

    mStopped = false;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    pthread_create(&mThread, &attr, ThreadWrapper, this);

    pthread_attr_destroy(&attr);

    mRunning = true;
}

#define INT64_MIN (-0x7fffffffffffffff - 1)
#define INT64_MAX 0x7fffffffffffffff
	
void TimedEventQueue::stop(bool flush) {
    if (!mRunning) {
        return;
    }

    if (flush) {
        postEventToBack(new StopEvent);
    } else {
        postTimedEvent(new StopEvent, INT64_MIN);
    }

    void *dummy;
    pthread_join(mThread, &dummy);

    mQueue.clear();

    mRunning = false;
}

TimedEventQueue::event_id TimedEventQueue::postEvent(Event* event) {
    // Reserve an earlier timeslot an INT64_MIN to be able to post
    // the StopEvent to the absolute head of the queue.
    return postTimedEvent(event, INT64_MIN + 1);
}

TimedEventQueue::event_id TimedEventQueue::postEventToBack(
        Event* event) {
    return postTimedEvent(event, INT64_MAX);
}

TimedEventQueue::event_id TimedEventQueue::postEventWithDelay(
        Event* event, int64_t delay_us) {
    if(delay_us < 0)
		return -1;
	
    return postTimedEvent(event, getCurrentTimeUS() + delay_us);
}

TimedEventQueue::event_id TimedEventQueue::postTimedEvent(
        Event* event, int64_t realtime_us) {
    Mutex::Autolock autoLock(mLock);

    event->setEventID(mNextEventID++);

    List<QueueItem>::iterator it = mQueue.begin();
    while (it != mQueue.end() && realtime_us >= (*it).realtime_us) {
        ++it;
    }

    QueueItem item;
    item.event = event;
    item.realtime_us = realtime_us;

    if (it == mQueue.begin()) {
        mQueueHeadChangedCondition.signal();
    }

    mQueue.insert(it, item);

    mQueueNotEmptyCondition.signal();

    return event->eventID();
}

static bool MatchesEventID(
        void *cookie, TimedEventQueue::Event* event) {
    TimedEventQueue::event_id *id =
        static_cast<TimedEventQueue::event_id *>(cookie);

    if (event->eventID() != *id) {
        return false;
    }

    *id = 0;

    return true;
}

bool TimedEventQueue::cancelEvent(event_id id) {
    if (id == 0) {
        return false;
    }

    cancelEvents(&MatchesEventID, &id, true /* stopAfterFirstMatch */);

    // if MatchesEventID found a match, it will have set id to 0
    // (which is not a valid event_id).

    return id == 0;
}

void TimedEventQueue::cancelEvents(
        bool (*predicate)(void *cookie, Event* event),
        void *cookie,
        bool stopAfterFirstMatch) {
    Mutex::Autolock autoLock(mLock);

    List<QueueItem>::iterator it = mQueue.begin();
    while (it != mQueue.end()) {
        if (!(*predicate)(cookie, (*it).event)) {
            ++it;
            continue;
        }

        if (it == mQueue.begin()) {
            mQueueHeadChangedCondition.signal();
        }

        //LOGI("cancelling event %d", (*it).event->eventID());

        (*it).event->setEventID(0);
        it = mQueue.erase(it);

        if (stopAfterFirstMatch) {
            return;
        }
    }
}

// static
void *TimedEventQueue::ThreadWrapper(void *me) {

    //androidSetThreadPriority(0, ANDROID_PRIORITY_FOREGROUND);

    static_cast<TimedEventQueue *>(me)->threadEntry();

    return NULL;
}

void TimedEventQueue::threadEntry() {
    for (;;) {
        int64_t now_us = 0;
        Event* event;

        {
            Mutex::Autolock autoLock(mLock);

            if (mStopped) {
                break;
            }

            while (mQueue.empty()) {
                mQueueNotEmptyCondition.wait(mLock);
            }

            event_id eventID = 0;
            for (;;) {
                if (mQueue.empty()) {
                    // The only event in the queue could have been cancelled
                    // while we were waiting for its scheduled time.
                    break;
                }

                List<QueueItem>::iterator it = mQueue.begin();
                eventID = (*it).event->eventID();

                now_us = getCurrentTimeUS();
                int64_t when_us = (*it).realtime_us;

                int64_t delay_us;
                if (when_us < 0 || when_us == INT64_MAX) {
                    delay_us = 0;
                } else {
                    delay_us = when_us - now_us;
                }

                if (delay_us <= 0) {
                    break;
                }

                static int64_t kMaxTimeoutUs = 10000000ll;  // 10 secs
                bool timeoutCapped = false;
                if (delay_us > kMaxTimeoutUs) {
                    ALOGE("delay_us exceeds max timeout: %lld us", delay_us);

                    // We'll never block for more than 10 secs, instead
                    // we will split up the full timeout into chunks of
                    // 10 secs at a time. This will also avoid overflow
                    // when converting from us to ns.
                    delay_us = kMaxTimeoutUs;
                    timeoutCapped = true;
                }

                int err = mQueueHeadChangedCondition.waitRelative(
                        mLock, delay_us * 1000ll);

                if (!timeoutCapped && err == -ETIMEDOUT) {
                    // We finally hit the time this event is supposed to
                    // trigger.
                    now_us = getCurrentTimeUS();
                    break;
                }
            }

            // The event w/ this id may have been cancelled while we're
            // waiting for its trigger-time, in that case
            // removeEventFromQueue_l will return NULL.
            // Otherwise, the QueueItem will be removed
            // from the queue and the referenced event returned.
            event = removeEventFromQueue_l(eventID);
        }

        if (event != NULL) {
            // Fire event with the lock NOT held.
            event->fire(this, now_us);
        }
    }
}

TimedEventQueue::Event* TimedEventQueue::removeEventFromQueue_l(
        event_id id) {
    for (List<QueueItem>::iterator it = mQueue.begin();
         it != mQueue.end(); ++it) {
        if ((*it).event->eventID() == id) {
            Event* event = (*it).event;
            event->setEventID(0);

            mQueue.erase(it);

            return event;
        }
    }

    ALOGW("Event %d was not found in the queue, already cancelled?", id);

    return NULL;
}

}  // namespace android

