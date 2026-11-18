// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CONNECT_MACHINE_H
#define BITCOIN_NODE_CONNECT_MACHINE_H

#include <util/machine.h>

#include <cstdint>

namespace node {
/**
 * A block of the connect window between its staging and its publication, as the
 * connector thread and the pool's workers see it of each other:
 *
 *   Running --Apply---------> Applied
 *      |
 *      +----ApplyDeferred---> Deferred --Claim--> Continuing --Finish--> Finished
 *
 * Running: the block's worker has it, and nobody else reads what it holds.
 * Applied: its outcome is complete, and its changes are in the store unless
 * it failed or could not be read. Deferred: its changes are in the store,
 * but transactions wait for inputs of lower blocks; nobody has it.
 * Continuing: a worker (any block's) finishes those transactions. Finished:
 * the outcome is complete.
 *
 * The state is written under the window's mutex. Whoever moves a block out of
 * Running or Continuing has written its outcome before, so a thread that reads
 * the state under the mutex may read what the state says exists.
 */
enum class WindowBlockState : uint8_t {
    Running,
    Applied,
    Deferred,
    Continuing,
    Finished,
};

enum class WindowBlockEvent : uint8_t {
    Apply,
    ApplyDeferred,
    Claim,
    Finish,
};

inline constexpr auto WINDOW_BLOCK_TABLE{util::MakeTransitionTable<WindowBlockState, WindowBlockEvent>({
    {WindowBlockState::Running, WindowBlockEvent::Apply, WindowBlockState::Applied},
    {WindowBlockState::Running, WindowBlockEvent::ApplyDeferred, WindowBlockState::Deferred},
    {WindowBlockState::Deferred, WindowBlockEvent::Claim, WindowBlockState::Continuing},
    {WindowBlockState::Continuing, WindowBlockEvent::Finish, WindowBlockState::Finished},
})};

//! The block's first pass is over, its changes in the store unless it failed: the blocks above it need
//! not wait for it.
constexpr bool IsApplied(WindowBlockState state) { return state != WindowBlockState::Running; }
//! The block's outcome is complete: the connector may publish it.
constexpr bool IsComplete(WindowBlockState state)
{
    return state == WindowBlockState::Applied || state == WindowBlockState::Finished;
}
//! A continuation is running on the block. (Whether its own worker still runs is its task's future.)
constexpr bool IsContinuing(WindowBlockState state) { return state == WindowBlockState::Continuing; }
} // namespace node

#endif // BITCOIN_NODE_CONNECT_MACHINE_H
