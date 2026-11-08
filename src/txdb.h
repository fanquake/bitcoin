// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include <coins.h>
#include <dbwrapper.h>
#include <kernel/caches.h>
#include <kernel/cs_main.h>
#include <sync.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class COutPoint;
class uint256;

//! User-controlled performance and debug options.
struct CoinsViewOptions {
    //! Maximum database write batch size in bytes.
    uint64_t batch_write_bytes{DEFAULT_DB_CACHE_BATCH};
    //! If non-zero, randomly exit when the database is flushed with (1/ratio) probability.
    int simulate_crash_ratio{0};
};

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB final : public CCoinsView
{
protected:
    DBParams m_db_params;
    CoinsViewOptions m_options;
    //! Prevents CompactFull() from using m_db while ResizeCache() replaces it.
    Mutex m_db_mutex;
    std::unique_ptr<CDBWrapper> m_db;
    std::shared_future<void> m_compaction;
    //! Guards m_open_marker.
    Mutex m_marker_mutex;
    //! The pair of heads (new, old) of the marker WriteRun() wrote last and left open: on disk, with the
    //! best block erased. Unset whenever that is not known to be what the database holds.
    std::optional<std::pair<uint256, uint256>> m_open_marker GUARDED_BY(m_marker_mutex);
public:
    explicit CCoinsViewDB(DBParams db_params, CoinsViewOptions options);
    ~CCoinsViewDB() override;

    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override EXCLUSIVE_LOCKS_REQUIRED(!m_marker_mutex);
    //! See CCoinsView::WriteRun. The marker (new_head, old_head) is written with the first batch and, unless
    //! the run is final, left in place. Nothing is assumed about a marker another object left in the
    //! database; the marker this object wrote last and left open is remembered, and a call with the same
    //! pair of heads writes its changes alone. The marker's keys sort below and above every coin's: a
    //! batch that carries them covers the whole range of keys whatever coins it holds, and the batches of
    //! a caller that writes neighbouring coins together are disjoint ranges only without them. Two threads
    //! may call it at once with the same pair of heads and neither final (the store's flush does), and the
    //! database takes the batches one after the other.
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override EXCLUSIVE_LOCKS_REQUIRED(!m_marker_mutex);
    //! Get a cursor to iterate over the whole state.
    std::unique_ptr<CCoinsViewCursor> Cursor() const;

    //! Whether an unsupported database format is used.
    bool NeedsUpgrade();
    size_t EstimateSize() const override;

    //! Dynamically alter the underlying leveldb cache size.
    void ResizeCache(size_t new_cache_size) EXCLUSIVE_LOCKS_REQUIRED(cs_main, !m_db_mutex);

    //! Perform a full compaction of the underlying LevelDB on a one-shot background thread.
    std::shared_future<void> CompactFullAsync() EXCLUSIVE_LOCKS_REQUIRED(cs_main, !m_db_mutex);

    //! Return an underlying LevelDB property value, if available.
    std::optional<std::string> GetDBProperty(const std::string& property);
};

#endif // BITCOIN_TXDB_H
