// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_NOTIFICATIONS_INTERFACE_H
#define BITCOIN_KERNEL_NOTIFICATIONS_INTERFACE_H

#include <cstdint>
#include <variant>

class CBlockIndex;
enum class SynchronizationState;
struct bilingual_str;

namespace kernel {

//! Result type for use with std::variant to indicate that an operation should be interrupted.
struct Interrupted{};
enum class Warning;


//! Simple result type for functions that need to propagate an interrupt status and don't have other return values.
using InterruptResult = std::variant<std::monostate, Interrupted>;

template <typename T>
bool IsInterrupted(const T& result)
{
    return std::holds_alternative<kernel::Interrupted>(result);
}

/**
 * A base class defining functions for notifying about certain kernel
 * events.
 */
class Notifications
{
public:
    virtual ~Notifications() = default;

    [[nodiscard]] virtual InterruptResult blockTip(SynchronizationState state, const CBlockIndex& index, double verification_progress) { return {}; }
    virtual void headerTip(SynchronizationState state, int64_t height, int64_t timestamp, bool presync) {}
    virtual void progress(const bilingual_str& title, int progress_percent, bool resume_possible) {}
    virtual void warningSet(Warning id, const bilingual_str& message) {}
    virtual void warningUnset(Warning id) {}

    //! The flush error notification is sent to notify the user that an error
    //! occurred while flushing block data to disk. Kernel code may ignore flush
    //! errors that don't affect the immediate operation it is trying to
    //! perform. Applications can choose to handle the flush error notification
    //! by logging the error, or notifying the user, or triggering an early
    //! shutdown as a precaution against causing more errors.
    //! After a failed flush of a block file or undo file, no later write-out
    //! of the block index and the chainstate succeeds in this run: what the
    //! failed flush left on disk is unknown, and the block index must not
    //! record a block whose data could be lost. An application that goes on
    //! after this notification can still read. It has to stop processing
    //! blocks and restart to write: a write-out that fails while a block is
    //! being connected leaves the coins one block ahead of the tip, and
    //! connecting a block again from there aborts.
    virtual void flushError(const bilingual_str& message) {}

    //! The fatal error notification is sent to notify the user when an error
    //! occurs in kernel code that can't be recovered from. After this
    //! notification is sent, whatever function triggered the error should also
    //! return an error code or raise an exception. Applications can choose to
    //! handle the fatal error notification by logging the error, or notifying
    //! the user, or triggering an early shutdown as a precaution against
    //! causing more errors.
    virtual void fatalError(const bilingual_str& message) {}
};
} // namespace kernel

#endif // BITCOIN_KERNEL_NOTIFICATIONS_INTERFACE_H
