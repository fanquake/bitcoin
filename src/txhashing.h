// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXHASHING_H
#define BITCOIN_TXHASHING_H

#include <cstddef>
#include <span>

class CBlock;
class ThreadPool;

/**
 * Deserialise a block in witness format from a contiguous buffer, computing every transaction's txid and wtxid
 * from the bytes it was read from rather than by serialising it again, and, when `pool` has workers and the block
 * has enough transactions to share out, hashing the transactions on those workers alongside this thread. The
 * digests are the ones CTransaction::ComputeHash() and ComputeWitnessHash() produce; test/txhashing_tests.cpp and
 * the `block` fuzz target hold the two paths equal on the same bytes.
 *
 * Returns the number of bytes consumed. Throws std::ios_base::failure on malformed or short input, as `>>` does,
 * leaving `block` in an unspecified state.
 */
size_t DeserializeBlock(std::span<const std::byte> data, CBlock& block, ThreadPool* pool);

#endif // BITCOIN_TXHASHING_H
