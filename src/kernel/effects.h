// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_EFFECTS_H
#define BITCOIN_KERNEL_EFFECTS_H

#include <sync.h>
#include <util/byte_ledger.h>
#include <util/lifecycle.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <thread>
#include <utility>

namespace kernel {

/**
 * Runs effects (system interactions that validation requests, such as writing
 * undo data) on one worker thread, in the order they were enqueued, and hands
 * each effect's completion back to be applied under the requester's lock.
 *
 * The split matters for locking: the worker never takes the requester's lock
 * itself, and WaitForIO() only waits for the I/O half, so a thread holding
 * that lock may wait for the I/O to finish and then apply the completions
 * itself. The worker calls the on_completion hook after each effect, which
 * is where the requester normally applies completions (taking its lock).
 *
 * Enqueue order is preserved end to end: effects run in order and their
 * completions are applied in that same order. The appliers keep the second
 * half: ApplyCompletions() runs each completion outside the executor's mutex,
 * so every thread that calls it holds one common lock, the requester's
 * (cs_main in the node).
 *
 * Whether it takes effects is one machine (util/lifecycle.h): Stopped until
 * Start(), Running until Stop(), Draining while Stop() waits for the I/O of
 * what was queued, then Stopped again. An effect queued while it is not
 * Running would never run, or run behind the stop that was to be the last
 * word: Enqueue() asserts.
 */
class EffectsExecutor
{
public:
    //! The I/O half, run on the worker with no lock held. Returns success.
    using Effect = std::function<bool()>;
    //! The completion half, run by ApplyCompletions() with the result of the I/O.
    using Completion = std::function<void(bool ok)>;

    //! `backlog` is the account the weight of the queued effects is kept in (util/byte_ledger.h).
    explicit EffectsExecutor(util::ByteAccount& backlog, std::function<void()> on_completion = {});
    ~EffectsExecutor() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    EffectsExecutor(const EffectsExecutor&) = delete;
    EffectsExecutor& operator=(const EffectsExecutor&) = delete;

    //! Start the worker. Nothing is queued before it (Enqueue()).
    void Start() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Run every enqueued effect's I/O, then stop the worker. Completions stay queued for ApplyCompletions().
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Queue an effect; `bytes` is its weight for the backlog limit. The executor is Running.
    void Enqueue(Effect effect, Completion completion, size_t bytes = 0) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Wait until the I/O of every enqueued effect has run. Safe with the requester's lock held.
    void WaitForIO() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Wait until the weight of effects whose I/O has not run yet is at most `limit_bytes`. Safe with the requester's
    //! lock held, as WaitForIO(); called without it where it can be, so that the lock is free while it waits.
    void WaitForBacklogBelow(size_t limit_bytes) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Wait until a completion is ready to be applied, until nothing is queued or running, or until `enough`
    //! holds, whichever is first: for a requester that waits for what the completions release and applies
    //! them itself (ApplyCompletions()) rather than leave them to the hook, which gives up where the
    //! requester's lock is taken. `enough` is asked under the executor's mutex, after each effect's I/O and
    //! after completions were applied, by any thread; it must not take the requester's lock. Called without
    //! that lock.
    void WaitForCompletionOr(const std::function<bool()>& enough) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Apply the completions of finished effects, in order. The caller holds whatever lock the completions need.
    //! A thread in WaitForCompletionOr() is woken once some were applied.
    void ApplyCompletions() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    size_t PendingBytes() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool HasPendingCompletions() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Whether nothing is left: no effect queued or running, no completion to apply.
    bool Idle() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct Item {
        Effect effect;
        Completion completion;
        size_t bytes;
    };

    void Loop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    mutable Mutex m_mutex;
    std::condition_variable m_cv_work;
    std::condition_variable m_cv_done;
    std::deque<Item> m_queue GUARDED_BY(m_mutex);
    std::deque<std::pair<Completion, bool>> m_done GUARDED_BY(m_mutex);
    //! The weight of the effects whose I/O has not run: changed under m_mutex, read by anyone.
    util::ByteAccount& m_backlog;
    bool m_io_running GUARDED_BY(m_mutex){false};
    util::Lifecycle m_life GUARDED_BY(m_mutex);
    std::thread m_thread;
    const std::function<void()> m_on_completion;
};

} // namespace kernel

#endif // BITCOIN_KERNEL_EFFECTS_H
