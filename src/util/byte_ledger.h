// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_BYTE_LEDGER_H
#define BITCOIN_UTIL_BYTE_LEDGER_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace util {

/**
 * The memory that blocks hold between their arrival and the moment nothing
 * of them is left to do, by where it is held. With blocks received, validated
 * and written beside each other every holder needs a bound of its own: the
 * ledger is the one list of the holders, so that one added later is added
 * here, with a limit or with the statement that it has none.
 *
 * An account is a count and a limit. Its holder changes the count under
 * whatever lock orders its own work, and asks Admits() before it takes more:
 * where the answer is no the holder waits or leaves the work for later, it
 * never fails. A limit of zero is the statement that the account has none: it
 * is named and counted, and bounded, if at all, by something that is not a
 * number of bytes (the comment of each account says by what).
 *
 * The counts are estimates where measuring would mean visiting a block
 * (each holder says which), and the ledger sets no budget: every limit is
 * its holder's.
 *
 * What the limits come to: the block cache's budget (-blockcache), an eighth
 * of the coins cache and 32 MiB at least for the connect window, the same
 * eighth and 64 MiB at least for the blocks pinned by signals that wait for
 * an undo write, and 64 MiB of undo backlog. Beside that sum, with no limit
 * in bytes: one block message, and the block a task makes of it, for each
 * connect pool worker but one, and the free nodes of the coin
 * store's regions, which the coins cache has counted at its largest. The
 * coin store's table of announced transaction ids, 4 MiB for each store
 * that connected a window block (CoinsStore::AnnounceCreates()), is
 * beside every account as well.
 */
enum class ByteAccountId : uint8_t {
    //! Accepted blocks kept until they are connected (node::BlockCache), as measured. Limit: its budget.
    BlockCache,
    //! The blocks of the connect window with their views of the coins: an estimate from each block's counts
    //! and known size, the worker's once it has applied the block. Limit: a share of the coins cache.
    Window,
    //! Undo data queued for writing and not yet written, by its serialized size. Limit: a constant.
    UndoBacklog,
    //! Connected blocks kept by a signal that waits for its undo write: an estimate from the block's counts.
    //! Limit: a share of the coins cache, at which the activating thread waits before it publishes another
    //! block. Not in a reorganisation's step, whose blocks' signals leave together at its end.
    //! The undo backlog's limit does not bound it: that one counts serialized undo data, and the blocks whose
    //! signals wait behind a full backlog are many times its size.
    PinnedBlocks,
};

inline constexpr std::array BYTE_ACCOUNTS{
    ByteAccountId::BlockCache,
    ByteAccountId::Window,
    ByteAccountId::UndoBacklog,
    ByteAccountId::PinnedBlocks,
};

//! One account. Safe to read from any thread; its holder orders the changes.
class ByteAccount
{
    std::atomic<size_t> m_bytes{0};
    std::atomic<size_t> m_limit{0};

public:
    void Add(size_t bytes) { m_bytes.fetch_add(bytes, std::memory_order_relaxed); }
    void Sub(size_t bytes) { m_bytes.fetch_sub(bytes, std::memory_order_relaxed); }
    //! For a holder that works its count out anew each time.
    void Set(size_t bytes) { m_bytes.store(bytes, std::memory_order_relaxed); }
    //! Zero: the account has no limit in bytes.
    void SetLimit(size_t limit) { m_limit.store(limit, std::memory_order_relaxed); }

    size_t Bytes() const { return m_bytes.load(std::memory_order_relaxed); }
    size_t Limit() const { return m_limit.load(std::memory_order_relaxed); }
    //! Whether the account stays within its limit with `more` bytes added. An account without a limit admits
    //! everything. The holder decides what it does with one that is empty and still does not admit.
    bool Admits(size_t more = 0) const
    {
        const size_t limit{Limit()};
        return limit == 0 || Bytes() + more <= limit;
    }
};

class ByteLedger
{
    std::array<ByteAccount, BYTE_ACCOUNTS.size()> m_accounts;

public:
    ByteAccount& operator[](ByteAccountId id) { return m_accounts[static_cast<size_t>(id)]; }
    const ByteAccount& operator[](ByteAccountId id) const { return m_accounts[static_cast<size_t>(id)]; }
};

} // namespace util

#endif // BITCOIN_UTIL_BYTE_LEDGER_H
