// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_TIME_H
#define BITCOIN_UTIL_TIME_H

// The `util/time.h` header is designed to be a drop-in replacement for `chrono`.
#include <atomic>
#include <chrono> // IWYU pragma: export
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

using namespace std::chrono_literals;

/// Version of the system clock that is mockable in the context of tests (via
/// FakeNodeClock or ::SetMockTime), otherwise the system clock.
struct NodeClock : public std::chrono::system_clock {
    using time_point = std::chrono::time_point<NodeClock>;
    /** Return current system time or mocked time, if set */
    static time_point now() noexcept;
    static std::time_t to_time_t(const time_point&) = delete; // unused
    static time_point from_time_t(std::time_t) = delete;      // unused
    static constexpr time_point epoch{};
};
using NodeSeconds = std::chrono::time_point<NodeClock, std::chrono::seconds>;

using SteadyClock = std::chrono::steady_clock;
using SteadySeconds = std::chrono::time_point<std::chrono::steady_clock, std::chrono::seconds>;
using SteadyMilliseconds = std::chrono::time_point<std::chrono::steady_clock, std::chrono::milliseconds>;
using SteadyMicroseconds = std::chrono::time_point<std::chrono::steady_clock, std::chrono::microseconds>;

using SystemClock = std::chrono::system_clock;

/// Version of SteadyClock that is mockable in the context of tests (via
/// FakeSteadyClock, or Self::SetMockTime), otherwise the system steady
/// clock.
struct MockableSteadyClock : public std::chrono::steady_clock {
    using time_point = std::chrono::time_point<MockableSteadyClock>;

    using mock_time_point = std::chrono::time_point<MockableSteadyClock, std::chrono::milliseconds>;
    static constexpr mock_time_point::duration INITIAL_MOCK_TIME{1};

    /** Return current system time or mocked time, if set */
    static time_point now() noexcept;
    static std::time_t to_time_t(const time_point&) = delete; // unused
    static time_point from_time_t(std::time_t) = delete;      // unused

    /** Set mock time for testing.
     * When mocking the steady clock, start at INITIAL_MOCK_TIME and add durations to elapse time as necessary
     * for testing.
     * To stop mocking, call ClearMockTime().
     */
    static void SetMockTime(mock_time_point::duration mock_time_in);

    /** Clear mock time, go back to system steady clock. */
    static void ClearMockTime();
};

void UninterruptibleSleep(const std::chrono::microseconds& n);

/**
 * Helper to count the seconds of a duration/time_point.
 *
 * All durations/time_points should be using std::chrono and calling this should generally
 * be avoided in code. Though, it is still preferred to an inline t.count() to
 * protect against a reliance on the exact type of t.
 *
 * This helper is used to convert durations/time_points before passing them over an
 * interface that doesn't support std::chrono (e.g. RPC, debug log, or the GUI)
 */
template <typename Dur1, typename Dur2>
constexpr auto Ticks(Dur2 d)
{
    return std::chrono::duration_cast<Dur1>(d).count();
}

template <typename Duration>
constexpr int64_t TicksSeconds(Duration d)
{
    return int64_t{Ticks<std::chrono::seconds>(d)};
}
template <typename Duration, typename Timepoint>
constexpr auto TicksSinceEpoch(Timepoint t)
{
    return Ticks<Duration>(t.time_since_epoch());
}
constexpr int64_t count_seconds(std::chrono::seconds t) { return t.count(); }
constexpr int64_t count_milliseconds(std::chrono::milliseconds t) { return t.count(); }
constexpr int64_t count_microseconds(std::chrono::microseconds t) { return t.count(); }

using HoursDouble = std::chrono::duration<double, std::chrono::hours::period>;
using SecondsDouble = std::chrono::duration<double, std::chrono::seconds::period>;
using MillisecondsDouble = std::chrono::duration<double, std::chrono::milliseconds::period>;

/**
 * The CPU time the calling thread has used so far, where the platform tells (zero elsewhere). Against the
 * wall time of the same stretch it says how long the thread was not running.
 */
std::chrono::nanoseconds ThreadCpuTime();

/**
 * How often the calling thread has gone to sleep of its own accord so far (its voluntary context switches:
 * a lock another thread holds, a wait, a read that blocks), where the platform tells (zero elsewhere).
 */
uint64_t ThreadSleeps();

/** What one kind of task cost the threads that ran it: counters any thread may add to. */
struct TaskTimes {
    std::atomic<int64_t> wall_ns{0};
    std::atomic<int64_t> cpu_ns{0};
    std::atomic<uint64_t> sleeps{0};
};

/** Adds the wall time, the CPU time and the sleeps of its scope to a TaskTimes. */
class TaskTimer
{
    TaskTimes& m_times;
    const SteadyClock::time_point m_wall_start{SteadyClock::now()};
    const std::chrono::nanoseconds m_cpu_start{ThreadCpuTime()};
    const uint64_t m_sleeps_start{ThreadSleeps()};

public:
    explicit TaskTimer(TaskTimes& times) : m_times{times} {}
    ~TaskTimer()
    {
        m_times.wall_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - m_wall_start).count(), std::memory_order_relaxed);
        m_times.cpu_ns.fetch_add((ThreadCpuTime() - m_cpu_start).count(), std::memory_order_relaxed);
        m_times.sleeps.fetch_add(ThreadSleeps() - m_sleeps_start, std::memory_order_relaxed);
    }
};

/**
 * DEPRECATED
 * Use either ClockType::now() or Now<TimePointType>() if a cast is needed.
 * ClockType is
 * - SteadyClock/std::chrono::steady_clock for steady time
 * - SystemClock/std::chrono::system_clock for system time
 * - NodeClock                             for mockable system time
 */
int64_t GetTime();

/** For testing. Set e.g. with the setmocktime rpc, or -mocktime argument */
void SetMockTime(std::chrono::seconds mock_time_in);
void SetMockTime(std::chrono::time_point<NodeClock, std::chrono::seconds> mock);

/** For testing */
std::chrono::seconds GetMockTime();

/**
 * Return the current time point cast to the given precision. Only use this
 * when an exact precision is needed, otherwise use T::clock::now() directly.
 */
template <typename T>
T Now()
{
    return std::chrono::time_point_cast<typename T::duration>(T::clock::now());
}
/** DEPRECATED, see GetTime */
template <typename T>
T GetTime()
{
    return Now<std::chrono::time_point<NodeClock, T>>().time_since_epoch();
}

/**
 * ISO 8601 formatting is preferred. Use the FormatISO8601{DateTime,Date}
 * helper functions if possible.
 */
std::string FormatISO8601DateTime(int64_t nTime);
std::string FormatISO8601Date(int64_t nTime);
std::optional<int64_t> ParseISO8601DateTime(std::string_view str);

/**
 * RFC1123 formatting https://www.rfc-editor.org/rfc/rfc1123#section-5.2.14
 * Used in HTTP/1.1 responses
 */
std::string FormatRFC1123DateTime(int64_t nTime);

/**
 * Convert milliseconds to a struct timeval for e.g. select.
 */
struct timeval MillisToTimeval(int64_t nTimeout);

/**
 * Convert milliseconds to a struct timeval for e.g. select.
 */
struct timeval MillisToTimeval(std::chrono::milliseconds ms);

#endif // BITCOIN_UTIL_TIME_H
