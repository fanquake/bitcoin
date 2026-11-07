// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/effects.h>

#include <chrono>
#include <util/check.h>
#include <util/threadnames.h>

namespace kernel {

EffectsExecutor::EffectsExecutor(std::function<void()> on_completion)
    : m_on_completion{std::move(on_completion)} {}

EffectsExecutor::~EffectsExecutor()
{
    Stop();
}

void EffectsExecutor::Start()
{
    Assert(!m_thread.joinable());
    WITH_LOCK(m_mutex, m_stop = false);
    m_thread = std::thread([this] { Loop(); });
}

void EffectsExecutor::Stop()
{
    {
        LOCK(m_mutex);
        m_stop = true;
    }
    m_cv_work.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

EffectsExecutor::Ticket EffectsExecutor::Enqueue(Effect effect, Completion completion, size_t bytes)
{
    Assert(m_thread.joinable()); // an effect queued on a never-started executor would wait forever
    Ticket ticket;
    {
        LOCK(m_mutex);
        m_queue.push_back(Item{std::move(effect), std::move(completion), bytes});
        m_pending_bytes += bytes;
        ticket = ++m_enqueued;
    }
    m_cv_work.notify_one();
    return ticket;
}

void EffectsExecutor::WaitForIO()
{
    WAIT_LOCK(m_mutex, lock);
    m_cv_done.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_queue.empty() && !m_io_running; });
}

void EffectsExecutor::WaitForIO(Ticket ticket)
{
    WAIT_LOCK(m_mutex, lock);
    m_cv_done.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_finished >= ticket; });
}

EffectsExecutor::Ticket EffectsExecutor::LastEnqueued() const
{
    LOCK(m_mutex);
    return m_enqueued;
}

void EffectsExecutor::WaitForBacklogBelow(size_t limit_bytes)
{
    WAIT_LOCK(m_mutex, lock);
    m_cv_done.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_pending_bytes <= limit_bytes; });
}

void EffectsExecutor::ApplyCompletions()
{
    while (true) {
        std::pair<Completion, bool> done;
        {
            LOCK(m_mutex);
            if (m_done.empty()) return;
            done = std::move(m_done.front());
            m_done.pop_front();
        }
        if (done.first) done.first(done.second);
    }
}

size_t EffectsExecutor::PendingBytes() const
{
    LOCK(m_mutex);
    return m_pending_bytes;
}

bool EffectsExecutor::HasPendingCompletions() const
{
    LOCK(m_mutex);
    return !m_done.empty();
}

void EffectsExecutor::Loop()
{
    util::ThreadRename("effects");
    while (true) {
        Item item;
        {
            WAIT_LOCK(m_mutex, lock);
            while (!m_stop && m_queue.empty()) {
                if (!m_done.empty() && m_on_completion) {
                    // Completions are waiting for the requester's lock, which may be free by now: retry the
                    // hook shortly rather than leaving them until the next effect or settle. The last block
                    // of a sync has no next effect to ride on, so without this its notifications would wait
                    // for the next settle.
                    m_cv_work.wait_for(lock, std::chrono::milliseconds{10});
                    if (!m_stop && m_queue.empty() && !m_done.empty()) {
                        REVERSE_LOCK(lock, m_mutex);
                        m_on_completion();
                    }
                } else {
                    m_cv_work.wait(lock);
                }
            }
            if (m_queue.empty()) return; // stopping, and every effect's I/O has run
            item = std::move(m_queue.front());
            m_queue.pop_front();
            m_io_running = true;
        }
        const bool ok{item.effect ? item.effect() : true};
        {
            LOCK(m_mutex);
            m_done.emplace_back(std::move(item.completion), ok);
            m_pending_bytes -= item.bytes;
            m_io_running = false;
            ++m_finished;
        }
        m_cv_done.notify_all();
        if (m_on_completion) m_on_completion();
    }
}

} // namespace kernel
