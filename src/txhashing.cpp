// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txhashing.h>

#include <primitives/block.h>
#include <primitives/transaction.h>
#include <primitives/txsegments.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <uint256.h>
#include <util/check.h>
#include <util/threadpool.h>

#include <algorithm>
#include <array>
#include <future>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

//! Transactions are parsed into chunks; a chunk is handed to a worker as soon as it is full, so the hashing of
//! one chunk overlaps the parsing of the next. The worker hashes from the block bytes and the chunk's byte
//! ranges, then builds the chunk's transaction objects from the parsed transactions it owns: it moves them,
//! which touches their headers, not the input and output records the parsing thread wrote. Chunks are sized by
//! bytes: every task wakes a worker, which costs a system call on each side, so a block should wake as many
//! workers as its size justifies and no more, whatever the pool's size.
constexpr size_t MAX_TXS_PER_CHUNK{256};
//! Fewer than this per chunk is not worth a hop to another thread.
constexpr size_t MIN_TXS_PER_CHUNK{32};
//! About this many bytes of block per task.
constexpr size_t BYTES_PER_CHUNK{128 * 1024};

struct Chunk {
    std::array<CMutableTransaction, MAX_TXS_PER_CHUNK> txs;
    std::array<TxSegments, MAX_TXS_PER_CHUNK> segments;
    std::array<CTransactionRef, MAX_TXS_PER_CHUNK> refs;
    size_t count{0};
    std::optional<std::future<void>> future;
};

void FinishChunk(std::span<const std::byte> data, Chunk& chunk)
{
    for (size_t i{0}; i < chunk.count; ++i) {
        const TxSegments& segments{chunk.segments[i]};
        const auto [txid, wtxid]{HashTransactionSegments(data, segments)};
        chunk.refs[i] = std::make_shared<const CTransaction>(CTransaction::PrecomputedHashes{}, std::move(chunk.txs[i]), txid, wtxid, segments.has_witness);
    }
}

//! Owns the chunks and waits for every one a worker is finishing, also when the parse throws, since the workers
//! reference the chunks.
class ChunkHasher
{
    std::span<const std::byte> m_data;
    ThreadPool* m_pool;
    size_t m_chunk_size;
    std::vector<std::unique_ptr<Chunk>> m_chunks;

public:
    ChunkHasher(std::span<const std::byte> data, ThreadPool* pool, size_t tx_count)
        : m_data{data}, m_pool{pool && pool->WorkersCount() > 0 ? pool : nullptr}
    {
        // As many transactions per chunk as make up about BYTES_PER_CHUNK of this block, within the bounds.
        const size_t bytes_per_tx{std::max<size_t>(1, data.size() / std::max<size_t>(1, tx_count))};
        m_chunk_size = std::clamp(BYTES_PER_CHUNK / bytes_per_tx, MIN_TXS_PER_CHUNK, MAX_TXS_PER_CHUNK);
    }
    ~ChunkHasher() { Wait(); }

    //! The chunk the next transaction goes into; dispatches a full chunk first.
    Chunk& Next()
    {
        if (m_chunks.empty() || m_chunks.back()->count == m_chunk_size) {
            if (!m_chunks.empty()) Dispatch(*m_chunks.back());
            m_chunks.push_back(std::make_unique<Chunk>());
        }
        return *m_chunks.back();
    }
    void Dispatch(Chunk& chunk)
    {
        if (m_pool && chunk.count >= MIN_TXS_PER_CHUNK) {
            if (auto future{m_pool->Submit([data = m_data, &chunk] { FinishChunk(data, chunk); })}) {
                chunk.future = std::move(*future);
                return;
            }
            // The pool stopped meanwhile: this thread finishes it.
        }
        FinishChunk(m_data, chunk);
    }
    //! Dispatch the last chunk, wait for all of them, and hand the transactions over in order.
    void Finish(std::vector<CTransactionRef>& vtx)
    {
        if (!m_chunks.empty() && !m_chunks.back()->future) Dispatch(*m_chunks.back());
        Wait();
        size_t total{0};
        for (const auto& chunk : m_chunks) total += chunk->count;
        vtx.reserve(total);
        for (auto& chunk : m_chunks) {
            for (size_t i{0}; i < chunk->count; ++i) vtx.push_back(std::move(chunk->refs[i]));
        }
    }
    void Wait() noexcept
    {
        for (auto& chunk : m_chunks) {
            if (chunk->future) {
                chunk->future->wait();
                chunk->future.reset();
            }
        }
    }
};

} // namespace

size_t DeserializeBlock(std::span<const std::byte> data, CBlock& block, ThreadPool* pool)
{
    block.SetNull();
    TxSegmentRecorder reader{data};
    reader >> static_cast<CBlockHeader&>(block);

    // The chunks are allocated as the transactions arrive, so a claimed count costs the sender the bytes.
    const size_t count{ReadCompactSize(reader)};
    ChunkHasher hasher{data, pool, count};
    for (size_t parsed{0}; parsed < count; ++parsed) {
        Chunk& chunk{hasher.Next()};
        reader.Begin(chunk.segments[chunk.count]);
        UnserializeTransaction(chunk.txs[chunk.count], reader, TX_WITH_WITNESS);
        reader.End();
        ++chunk.count;
    }
    hasher.Finish(block.vtx);
    return data.size() - reader.size();
}
