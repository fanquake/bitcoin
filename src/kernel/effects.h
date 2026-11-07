// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_EFFECTS_H
#define BITCOIN_KERNEL_EFFECTS_H

#include <sync.h>

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
 * completions are applied in that same order.
 */
class EffectsExecutor
{
public:
    //! The I/O half, run on the worker with no lock held. Returns success.
    using Effect = std::function<bool()>;
    //! The completion half, run by ApplyCompletions() with the result of the I/O.
    using Completion = std::function<void(bool ok)>;

    explicit EffectsExecutor(std::function<void()> on_completion = {});
    ~EffectsExecutor() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    EffectsExecutor(const EffectsExecutor&) = delete;
    EffectsExecutor& operator=(const EffectsExecutor&) = delete;

    //! Start the worker. Effects enqueued before this wait for it.
    void Start() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Run every enqueued effect's I/O, then stop the worker. Completions stay queued for ApplyCompletions().
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! A position in the queue: Enqueue() returns the ticket of the effect it added.
    using Ticket = uint64_t;

    //! Queue an effect; `bytes` is its weight for the backlog limit.
    Ticket Enqueue(Effect effect, Completion completion, size_t bytes = 0) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Wait until the I/O of every enqueued effect has run. Safe with the requester's lock held.
    void WaitForIO() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Wait until the I/O of the effect with `ticket` and of every effect before it has run.
    void WaitForIO(Ticket ticket) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The ticket of the most recently enqueued effect (0 if none).
    Ticket LastEnqueued() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Wait until the weight of effects whose I/O has not run yet is at most `limit_bytes`. Call without the requester's lock.
    void WaitForBacklogBelow(size_t limit_bytes) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Apply the completions of finished effects, in order. The caller holds whatever lock the completions need.
    void ApplyCompletions() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    size_t PendingBytes() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool HasPendingCompletions() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

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
    size_t m_pending_bytes GUARDED_BY(m_mutex){0};
    Ticket m_enqueued GUARDED_BY(m_mutex){0};
    //! Effects run in order, so this is the ticket of the last effect whose I/O has run.
    Ticket m_finished GUARDED_BY(m_mutex){0};
    bool m_io_running GUARDED_BY(m_mutex){false};
    bool m_stop GUARDED_BY(m_mutex){false};
    std::thread m_thread;
    const std::function<void()> m_on_completion;
};

} // namespace kernel

#endif // BITCOIN_KERNEL_EFFECTS_H
