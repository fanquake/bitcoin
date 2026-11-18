// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_LIFECYCLE_H
#define BITCOIN_UTIL_LIFECYCLE_H

#include <sync.h>
#include <util/check.h>
#include <util/machine.h>

#include <cstdint>
#include <utility>

namespace util {
/**
 * Whether something that takes work from other threads still takes it:
 *
 *   Stopped --Start--> Running --Drain--> Draining --Finish--> Stopped
 *
 * Running: work is accepted. Draining: none is accepted any more, and the
 * work accepted before is being finished; whoever stops the object waits for
 * that. Stopped: nothing is accepted and nothing is left; what the object's
 * work reached for may be destroyed.
 *
 * Each object that takes work from another thread says it with this machine:
 *
 *  - the effects executor (kernel::EffectsExecutor): an effect queued while
 *    it is not Running is a bug of the caller and asserts;
 *  - the connector thread (ChainstateManager::SignalConnector()): a block
 *    handed over while it is not Running is stored and not activated;
 *  - the receive tasks (PeerManagerImpl::HandOffBlock()): a block handed
 *    over while they are not Running asserts, and a task touches nothing of
 *    its owner once it has left the owner's task set, which is what the
 *    owner waits for while Draining;
 *  - a hook called from another thread (HookTarget): its owner disarms it
 *    and waits for a call that is running before it is destroyed.
 */
enum class LifeState : uint8_t {
    Stopped,
    Running,
    Draining,
};

enum class LifeEvent : uint8_t {
    Start,
    Drain,
    Finish,
};

inline constexpr auto LIFECYCLE_TABLE{MakeTransitionTable<LifeState, LifeEvent>({
    {LifeState::Stopped, LifeEvent::Start, LifeState::Running},
    {LifeState::Running, LifeEvent::Drain, LifeState::Draining},
    {LifeState::Draining, LifeEvent::Finish, LifeState::Stopped},
})};

//! The state of one such object. Not synchronized: the owner's mutex guards it.
class Lifecycle
{
    LifeState m_state;

    void Apply(LifeEvent event) { m_state = *Assert(LIFECYCLE_TABLE.Next(m_state, event)); }

public:
    constexpr explicit Lifecycle(LifeState initial = LifeState::Stopped) : m_state{initial} {}

    LifeState State() const { return m_state; }
    //! Whether new work is accepted.
    bool Running() const { return m_state == LifeState::Running; }
    bool Stopped() const { return m_state == LifeState::Stopped; }

    void Start() { Apply(LifeEvent::Start); }
    void Drain() { Apply(LifeEvent::Drain); }
    void Finish() { Apply(LifeEvent::Finish); }
    //! Drain, for a stop that may be asked for twice. Returns whether this call began the stop: the caller
    //! that did waits for the work and calls Finish().
    bool DrainIfRunning()
    {
        if (!Running()) return false;
        Drain();
        return true;
    }
};

/**
 * What a hook that another object calls, on any thread, reaches of its owner.
 * The hook holds the target by shared pointer and so never outlives it; the
 * owner is reached only through Run(), which refuses once the owner disarmed
 * the target. Disarm() waits for a call that is running, so after it returns
 * no hook is in the owner and none enters it again. The owner may go before
 * the object that calls the hook or after it, and does not call into it as it
 * goes.
 */
template <typename Owner>
class HookTarget
{
    Mutex m_mutex;
    Lifecycle m_life GUARDED_BY(m_mutex){LifeState::Running};
    Owner& m_owner GUARDED_BY(m_mutex);

public:
    explicit HookTarget(Owner& owner) : m_owner{owner} {}

    //! Call `fn` with the owner unless the target is disarmed, and return whether it was called. The owner is
    //! not destroyed while `fn` runs. `fn` takes no lock that a thread calling Disarm() may hold, unless the
    //! caller of Run() took that lock before it entered the target.
    template <typename Fn>
    bool Run(Fn&& fn) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        if (!m_life.Running()) return false;
        std::forward<Fn>(fn)(m_owner);
        return true;
    }

    //! Refuse every later call and wait for one that is running. Idempotent.
    void Disarm() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        // Taking the mutex is the wait: a running call holds it.
        LOCK(m_mutex);
        if (m_life.DrainIfRunning()) m_life.Finish();
    }
};
} // namespace util

#endif // BITCOIN_UTIL_LIFECYCLE_H
