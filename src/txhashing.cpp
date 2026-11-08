// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txhashing.h>

#include <crypto/sha256.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
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

//! Where one transaction lies in the block buffer: the whole transaction (the wtxid preimage) and the segments of
//! it that form the txid preimage, which leave out the segwit marker, flag and witness.
struct TxSegments {
    size_t begin{0};
    size_t end{0};
    //! At most three: up to the marker, from the flag to the witness, after the witness.
    std::array<std::pair<size_t, size_t>, 3> txid{};
    size_t txid_count{0};
    //! The segwit framing was present. After a successful parse that is the same as CTransaction::HasWitness(),
    //! since a framing with only empty witness stacks is rejected.
    bool has_witness{false};
};

//! A reader over the block buffer that records, through the TxidHashingStream calls UnserializeTransaction makes,
//! which bytes of each transaction form its txid preimage. Nothing is hashed here; see HashChunk().
class TxSegmentRecorder
{
    SpanReader m_reader;
    const size_t m_total;
    TxSegments* m_segments{nullptr};
    size_t m_segment_start{0};
    size_t m_hold_start{0};

    size_t Pos() const { return m_total - m_reader.size(); }
    void Close(size_t end)
    {
        Assert(m_segments->txid_count < m_segments->txid.size());
        m_segments->txid[m_segments->txid_count++] = {m_segment_start, end};
    }

public:
    explicit TxSegmentRecorder(std::span<const std::byte> data) : m_reader{data}, m_total{data.size()} {}

    void read(std::span<std::byte> dst) { m_reader.read(dst); }
    void ignore(size_t num_bytes) { m_reader.ignore(num_bytes); }
    size_t size() const { return m_reader.size(); }
    bool empty() const { return m_reader.empty(); }

    template <typename T>
    TxSegmentRecorder& operator>>(T&& obj)
    {
        ::Unserialize(*this, obj);
        return *this;
    }

    void Begin(TxSegments& segments)
    {
        segments = TxSegments{};
        m_segments = &segments;
        segments.begin = m_segment_start = Pos();
    }
    void End()
    {
        Close(Pos());
        m_segments->end = Pos();
        m_segments = nullptr;
    }

    void TxidHold() { m_hold_start = Pos(); }
    void TxidCommit() {}
    void TxidDiscard()
    {
        m_segments->has_witness = true;
        Close(m_hold_start);
        m_segment_start = Pos();
    }
    void TxidPause() { Close(Pos()); }
    void TxidResume() { m_segment_start = Pos(); }
};

uint256 Finish(CSHA256& hasher)
{
    unsigned char out[CSHA256::OUTPUT_SIZE];
    hasher.Finalize(out);
    CSHA256{}.Write(out, sizeof(out)).Finalize(out);
    return uint256{out};
}

std::pair<Txid, Wtxid> HashTransaction(std::span<const std::byte> data, const TxSegments& segments)
{
    const auto* bytes{reinterpret_cast<const unsigned char*>(data.data())};
    CSHA256 txid_hasher;
    for (size_t i{0}; i < segments.txid_count; ++i) {
        const auto [begin, end]{segments.txid[i]};
        txid_hasher.Write(bytes + begin, end - begin);
    }
    const Txid txid{Txid::FromUint256(Finish(txid_hasher))};
    if (!segments.has_witness) return {txid, Wtxid::FromUint256(txid.ToUint256())};
    CSHA256 wtxid_hasher;
    wtxid_hasher.Write(bytes + segments.begin, segments.end - segments.begin);
    return {txid, Wtxid::FromUint256(Finish(wtxid_hasher))};
}

//! Transactions are hashed in chunks of this many, each chunk handed to a worker as soon as it is parsed, so the
//! hashing overlaps the parsing of the next chunk. A worker reads only the block bytes and the chunk's segment
//! records, never the parsed transactions, which keeps it off the cache lines the parsing thread is writing.
constexpr size_t TXS_PER_CHUNK{128};
//! A chunk with fewer transactions than this (the last one) is hashed by the parsing thread.
constexpr size_t MIN_TXS_PER_TASK{32};

struct Chunk {
    std::array<TxSegments, TXS_PER_CHUNK> segments;
    std::array<std::pair<Txid, Wtxid>, TXS_PER_CHUNK> hashes;
    size_t count{0};
    std::optional<std::future<void>> future;
};

void HashChunk(std::span<const std::byte> data, Chunk& chunk)
{
    for (size_t i{0}; i < chunk.count; ++i) chunk.hashes[i] = HashTransaction(data, chunk.segments[i]);
}

//! Waits for every chunk a worker is hashing, also when the parse throws, since the workers reference the chunks.
class ChunkHasher
{
    std::span<const std::byte> m_data;
    ThreadPool* m_pool;
    std::vector<std::unique_ptr<Chunk>> m_chunks;

public:
    ChunkHasher(std::span<const std::byte> data, ThreadPool* pool) : m_data{data}, m_pool{pool && pool->WorkersCount() > 0 ? pool : nullptr} {}
    ~ChunkHasher() { Wait(); }

    //! The chunk the next transaction's segments go into; dispatches a full chunk first.
    Chunk& Next()
    {
        if (m_chunks.empty() || m_chunks.back()->count == TXS_PER_CHUNK) {
            if (!m_chunks.empty()) Dispatch(*m_chunks.back());
            m_chunks.push_back(std::make_unique<Chunk>());
        }
        return *m_chunks.back();
    }
    void Dispatch(Chunk& chunk)
    {
        if (m_pool && chunk.count >= MIN_TXS_PER_TASK) {
            if (auto future{m_pool->Submit([data = m_data, &chunk] { HashChunk(data, chunk); })}) {
                chunk.future = std::move(*future);
                return;
            }
            // The pool stopped meanwhile: this thread hashes.
        }
        HashChunk(m_data, chunk);
    }
    //! Dispatch the last chunk and wait for all of them.
    void Finish()
    {
        if (!m_chunks.empty() && !m_chunks.back()->future) Dispatch(*m_chunks.back());
        Wait();
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
    const std::pair<Txid, Wtxid>& Hashes(size_t index) const { return m_chunks[index / TXS_PER_CHUNK]->hashes[index % TXS_PER_CHUNK]; }
};

} // namespace

size_t DeserializeBlock(std::span<const std::byte> data, CBlock& block, ThreadPool* pool)
{
    block.SetNull();
    TxSegmentRecorder reader{data};
    reader >> static_cast<CBlockHeader&>(block);

    // Allocate as the transactions arrive, in the same batches as the generic vector deserialiser, so a claimed
    // count costs the sender the bytes.
    const size_t count{ReadCompactSize(reader)};
    std::vector<CMutableTransaction> txs;
    ChunkHasher hasher{data, pool};
    size_t allocated{0};
    while (allocated < count) {
        allocated = std::min(count, allocated + MAX_VECTOR_ALLOCATE / sizeof(CMutableTransaction));
        txs.reserve(allocated);
        while (txs.size() < allocated) {
            Chunk& chunk{hasher.Next()};
            txs.emplace_back();
            reader.Begin(chunk.segments[chunk.count]);
            UnserializeTransaction(txs.back(), reader, TX_WITH_WITNESS);
            reader.End();
            ++chunk.count;
        }
    }
    hasher.Finish();

    block.vtx.reserve(txs.size());
    for (size_t i{0}; i < txs.size(); ++i) {
        const auto& [txid, wtxid]{hasher.Hashes(i)};
        block.vtx.push_back(std::make_shared<const CTransaction>(CTransaction::PrecomputedHashes{}, std::move(txs[i]), txid, wtxid));
    }
    return data.size() - reader.size();
}
