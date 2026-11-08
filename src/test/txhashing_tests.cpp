// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hash.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <txhashing.h>
#include <uint256.h>
#include <util/threadpool.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

/**
 * DeserializeBlock() computes every transaction's txid and wtxid from the bytes it was read from instead of by
 * serialising it again. These tests hold it equal to the generic deserialiser, which does serialise again, on
 * random transactions, on mutated and truncated bytes, and on whole blocks with and without the hashing shared
 * over a thread pool.
 */
BOOST_FIXTURE_TEST_SUITE(txhashing_tests, BasicTestingSetup)

namespace {

CScript RandomScript(FastRandomContext& rng, size_t max_size)
{
    const auto bytes{rng.randbytes(rng.randrange(max_size + 1))};
    return CScript{bytes.begin(), bytes.end()};
}

//! A transaction with 0 to 5 inputs and outputs, each input with a witness stack with probability one half when
//! witnesses are wanted. Input-less and output-less transactions are produced on purpose.
CMutableTransaction RandomTransaction(FastRandomContext& rng, bool with_witness)
{
    CMutableTransaction tx;
    tx.version = rng.rand32();
    tx.nLockTime = rng.rand32();
    const size_t inputs{rng.randrange(6)};
    for (size_t i{0}; i < inputs; ++i) {
        CTxIn in;
        in.prevout = COutPoint{Txid::FromUint256(rng.rand256()), rng.rand32()};
        in.scriptSig = RandomScript(rng, 80);
        in.nSequence = rng.rand32();
        if (with_witness && rng.randbool()) {
            const size_t items{1 + rng.randrange(3)};
            for (size_t j{0}; j < items; ++j) {
                in.scriptWitness.stack.push_back(rng.randbytes(rng.randrange(72)));
            }
        }
        tx.vin.push_back(std::move(in));
    }
    const size_t outputs{rng.randrange(6)};
    for (size_t i{0}; i < outputs; ++i) {
        tx.vout.emplace_back(rng.randrange(COIN), RandomScript(rng, 60));
    }
    return tx;
}

//! The hashes as the generic path computes them: serialise again and hash.
Txid ReserializedTxid(const CTransaction& tx)
{
    return Txid::FromUint256((HashWriter{} << TX_NO_WITNESS(tx)).GetHash());
}
Wtxid ReserializedWtxid(const CTransaction& tx)
{
    if (!tx.HasWitness()) return Wtxid::FromUint256(ReserializedTxid(tx).ToUint256());
    return Wtxid::FromUint256((HashWriter{} << TX_WITH_WITNESS(tx)).GetHash());
}

template <typename Obj>
std::vector<std::byte> Bytes(const Obj& obj)
{
    DataStream stream;
    stream << obj;
    return {stream.begin(), stream.end()};
}

//! A block holding exactly these serialised transaction bytes, so a single transaction's bytes can be put through
//! the block deserialiser.
std::vector<std::byte> BlockAround(std::span<const std::byte> tx_bytes)
{
    std::vector<std::byte> bytes{Bytes(CBlockHeader{})};
    bytes.push_back(std::byte{1});
    bytes.insert(bytes.end(), tx_bytes.begin(), tx_bytes.end());
    return bytes;
}

//! Deserialise `bytes` as a block with the generic path and with DeserializeBlock (with and without a pool); the
//! paths must fail together or agree on everything. Returns whether they parsed.
bool CheckBlockBytes(std::span<const std::byte> bytes, ThreadPool* pool)
{
    bool generic_ok{true};
    CBlock generic;
    size_t generic_consumed{0};
    try {
        SpanReader reader{bytes};
        reader >> TX_WITH_WITNESS(generic);
        generic_consumed = bytes.size() - reader.size();
    } catch (const std::ios_base::failure&) {
        generic_ok = false;
    }

    for (ThreadPool* p : {static_cast<ThreadPool*>(nullptr), pool}) {
        bool hashed_ok{true};
        CBlock hashed;
        size_t hashed_consumed{0};
        try {
            hashed_consumed = DeserializeBlock(bytes, hashed, p);
        } catch (const std::ios_base::failure&) {
            hashed_ok = false;
        }
        BOOST_CHECK_EQUAL(generic_ok, hashed_ok);
        if (!generic_ok || !hashed_ok) continue;
        BOOST_CHECK_EQUAL(generic_consumed, hashed_consumed);
        BOOST_CHECK(hashed.GetHash() == generic.GetHash());
        BOOST_CHECK_EQUAL(hashed.vtx.size(), generic.vtx.size());
        for (size_t i{0}; i < std::min(hashed.vtx.size(), generic.vtx.size()); ++i) {
            BOOST_CHECK(hashed.vtx[i]->GetHash() == generic.vtx[i]->GetHash());
            BOOST_CHECK(hashed.vtx[i]->GetWitnessHash() == generic.vtx[i]->GetWitnessHash());
            BOOST_CHECK(hashed.vtx[i]->GetHash() == ReserializedTxid(*hashed.vtx[i]));
            BOOST_CHECK(hashed.vtx[i]->GetWitnessHash() == ReserializedWtxid(*hashed.vtx[i]));
            BOOST_CHECK_EQUAL(hashed.vtx[i]->HasWitness(), generic.vtx[i]->HasWitness());
        }
        // What was parsed serialises back to what was consumed.
        const std::vector<std::byte> again{Bytes(TX_WITH_WITNESS(hashed))};
        BOOST_CHECK(std::equal(again.begin(), again.end(), bytes.begin(), bytes.begin() + hashed_consumed));
    }
    return generic_ok;
}

} // namespace

BOOST_AUTO_TEST_CASE(transaction_hashes_from_bytes)
{
    size_t parsed_with_witness{0}, parsed_without{0}, misparsed{0};
    for (int i{0}; i < 1500; ++i) {
        const CMutableTransaction tx{RandomTransaction(m_rng, /*with_witness=*/m_rng.randbool())};
        if (CheckBlockBytes(BlockAround(Bytes(TX_WITH_WITNESS(tx))), nullptr)) {
            if (tx.HasWitness()) ++parsed_with_witness; else ++parsed_without;
        } else {
            // An input-less transaction with outputs is misread as segwit framing by both paths alike.
            BOOST_CHECK(tx.vin.empty() && !tx.vout.empty());
            ++misparsed;
        }
    }
    BOOST_CHECK(parsed_with_witness > 100);
    BOOST_CHECK(parsed_without > 100);
    BOOST_CHECK(misparsed > 0);
}

BOOST_AUTO_TEST_CASE(transaction_hashes_on_damaged_bytes)
{
    for (int i{0}; i < 1500; ++i) {
        const CMutableTransaction tx{RandomTransaction(m_rng, /*with_witness=*/true)};
        std::vector<std::byte> bytes{Bytes(TX_WITH_WITNESS(tx))};
        // Flip a few bytes, or cut the serialisation short: the paths fail together or agree.
        if (m_rng.randbool()) {
            const size_t flips{1 + m_rng.randrange(3)};
            for (size_t f{0}; f < flips; ++f) {
                bytes[m_rng.randrange(bytes.size())] = std::byte{static_cast<unsigned char>(m_rng.randbits(8))};
            }
        } else {
            bytes.resize(m_rng.randrange(bytes.size()));
        }
        CheckBlockBytes(BlockAround(bytes), nullptr);
    }
}

BOOST_AUTO_TEST_CASE(transaction_hashes_fixed_cases)
{
    // Version 1, no inputs, no outputs, locktime 0: with witness support the two zero bytes are read as a dummy
    // and a zero flag, and they are the vin and vout counts of the txid preimage.
    const std::vector<std::byte> empty_tx{Bytes(TX_WITH_WITNESS(CMutableTransaction{}))};
    BOOST_CHECK_EQUAL(empty_tx.size(), 10U);
    BOOST_CHECK(CheckBlockBytes(BlockAround(empty_tx), nullptr));

    // One input with a witness, no outputs; and one input without a witness next to one with.
    CMutableTransaction one_in;
    one_in.vin.emplace_back(COutPoint{Txid::FromUint256(m_rng.rand256()), 0});
    one_in.vin.back().scriptWitness.stack.push_back(m_rng.randbytes(33));
    BOOST_CHECK(CheckBlockBytes(BlockAround(Bytes(TX_WITH_WITNESS(one_in))), nullptr));
    one_in.vin.emplace_back(COutPoint{Txid::FromUint256(m_rng.rand256()), 1});
    one_in.vout.emplace_back(1, CScript{} << OP_TRUE);
    BOOST_CHECK(CheckBlockBytes(BlockAround(Bytes(TX_WITH_WITNESS(one_in))), nullptr));

    // A witness serialisation whose witness stacks are all empty is rejected by both paths alike.
    std::vector<std::byte> superfluous{Bytes(TX_NO_WITNESS(one_in))};
    superfluous.insert(superfluous.begin() + 4, {std::byte{0}, std::byte{1}});
    for (size_t i{0}; i < one_in.vin.size(); ++i) superfluous.insert(superfluous.end() - 4, std::byte{0});
    BOOST_CHECK(!CheckBlockBytes(BlockAround(superfluous), nullptr));
}

BOOST_AUTO_TEST_CASE(block_hashes_from_bytes)
{
    ThreadPool pool{"test"};
    pool.Start(3);

    for (const size_t tx_count : {1, 10, 127, 128, 129, 300, 1000}) {
        CBlock block;
        block.nVersion = m_rng.rand32();
        block.hashPrevBlock = m_rng.rand256();
        block.hashMerkleRoot = m_rng.rand256();
        block.nTime = m_rng.rand32();
        block.nBits = m_rng.rand32();
        block.nNonce = m_rng.rand32();
        for (size_t i{0}; i < tx_count; ++i) {
            CMutableTransaction tx;
            do {
                tx = RandomTransaction(m_rng, /*with_witness=*/true);
            } while (tx.vin.empty()); // the input-less forms do not round-trip in witness format
            block.vtx.push_back(MakeTransactionRef(std::move(tx)));
        }
        std::vector<std::byte> bytes{Bytes(TX_WITH_WITNESS(block))};
        const size_t block_size{bytes.size()};
        bytes.insert(bytes.end(), {std::byte{0xde}, std::byte{0xad}}); // trailing bytes are left unread

        BOOST_CHECK(CheckBlockBytes(bytes, &pool));
        CBlock hashed;
        BOOST_CHECK_EQUAL(DeserializeBlock(bytes, hashed, &pool), block_size);
        BOOST_CHECK_EQUAL(hashed.vtx.size(), tx_count);
        for (size_t i{0}; i < tx_count; ++i) {
            BOOST_CHECK(hashed.vtx[i]->GetHash() == block.vtx[i]->GetHash());
            BOOST_CHECK(hashed.vtx[i]->GetWitnessHash() == block.vtx[i]->GetWitnessHash());
        }

        // Short input fails as the generic path does, also while workers hold chunks of it.
        for (int i{0}; i < 10; ++i) {
            CheckBlockBytes(std::span<const std::byte>{bytes.data(), m_rng.randrange(block_size)}, &pool);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
