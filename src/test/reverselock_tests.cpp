// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sync.h>
#include <test/util/common.h>

#include <test/util/framework.h>

#include <stdexcept>

TEST_SUITE_BEGIN(reverselock_tests)

TEST_CASE(reverselock_basics)
{
    Mutex mutex;
    WAIT_LOCK(mutex, lock);

    CHECK(lock.owns_lock());
    AssertLockHeld(mutex);
    {
        REVERSE_LOCK(lock, mutex);
        AssertLockNotHeld(mutex);
        CHECK(!lock.owns_lock());
    }
    CHECK(lock.owns_lock());
}

TEST_CASE(reverselock_multiple)
{
    Mutex mutex2;
    Mutex mutex;
    WAIT_LOCK(mutex2, lock2);
    WAIT_LOCK(mutex, lock);

    // Make sure undoing two locks succeeds
    {
        REVERSE_LOCK(lock, mutex);
        CHECK(!lock.owns_lock());
        REVERSE_LOCK(lock2, mutex2);
        CHECK(!lock2.owns_lock());
    }
    CHECK(lock.owns_lock());
    CHECK(lock2.owns_lock());
}

TEST_CASE(reverselock_errors)
{
    Mutex mutex2;
    Mutex mutex;
    WAIT_LOCK(mutex2, lock2);
    WAIT_LOCK(mutex, lock);

#ifdef DEBUG_LOCKORDER
    bool prev = g_debug_lockorder_abort;
    g_debug_lockorder_abort = false;

    // Make sure trying to reverse lock a previous lock fails
    CHECK_EXCEPTION(REVERSE_LOCK(lock2, mutex2), std::logic_error, HasReason("mutex2 was not most recent critical section locked"));
    CHECK(lock2.owns_lock());

    g_debug_lockorder_abort = prev;
#endif

    // Make sure trying to reverse lock an unlocked lock fails
    lock.unlock();

    CHECK(!lock.owns_lock());

    bool failed = false;
    try {
        REVERSE_LOCK(lock, mutex);
    } catch(...) {
        failed = true;
    }

    CHECK(failed);
    CHECK(!lock.owns_lock());

    // Locking the original lock after it has been taken by a reverse lock
    // makes no sense. Ensure that the original lock no longer owns the lock
    // after giving it to a reverse one.

    lock.lock();
    CHECK(lock.owns_lock());
    {
        REVERSE_LOCK(lock, mutex);
        CHECK(!lock.owns_lock());
    }

    CHECK(failed);
    CHECK(lock.owns_lock());
}

TEST_SUITE_END()
