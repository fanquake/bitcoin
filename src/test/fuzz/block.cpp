// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <core_memusage.h>
#include <pubkey.h>
#include <streams.h>
#include <test/fuzz/fuzz.h>
#include <txhashing.h>
#include <util/chaintype.h>
#include <util/check.h>
#include <validation.h>

#include <string>

void initialize_block()
{
    SelectParams(ChainType::REGTEST);
}

FUZZ_TARGET(block, .init = initialize_block)
{
    CBlock block;
    bool deserialized{true};
    size_t consumed{0};
    try {
        SpanReader reader{buffer};
        reader >> TX_WITH_WITNESS(block);
        consumed = buffer.size() - reader.size();
    } catch (const std::ios_base::failure&) {
        deserialized = false;
    }
    // The byte-hashing block deserialiser (txhashing.h) must agree with the generic one on the same bytes: the same
    // outcome, the same bytes consumed, and the same transaction hashes, which it computes without serialising again.
    {
        CBlock hashed;
        bool hashed_deserialized{true};
        size_t hashed_consumed{0};
        try {
            hashed_consumed = DeserializeBlock(std::as_bytes(buffer), hashed, nullptr);
        } catch (const std::ios_base::failure&) {
            hashed_deserialized = false;
        }
        assert(hashed_deserialized == deserialized);
        if (deserialized) {
            assert(hashed_consumed == consumed);
            assert(hashed.GetHash() == block.GetHash());
            assert(hashed.vtx.size() == block.vtx.size());
            for (size_t i{0}; i < block.vtx.size(); ++i) {
                assert(hashed.vtx[i]->GetHash() == block.vtx[i]->GetHash());
                assert(hashed.vtx[i]->GetWitnessHash() == block.vtx[i]->GetWitnessHash());
                assert(hashed.vtx[i]->GetHash() == CMutableTransaction{*block.vtx[i]}.GetHash());
            }
        }
    }
    if (!deserialized) return;
    const Consensus::Params& consensus_params = Params().GetConsensus();
    BlockValidationState validation_state_pow_and_merkle;
    const bool valid_incl_pow_and_merkle = CheckBlock(block, validation_state_pow_and_merkle, consensus_params, /* fCheckPOW= */ true, /* fCheckMerkleRoot= */ true);
    assert(validation_state_pow_and_merkle.IsValid() || validation_state_pow_and_merkle.IsInvalid() || validation_state_pow_and_merkle.IsError());
    (void)validation_state_pow_and_merkle.Error("");
    BlockValidationState validation_state_pow;
    const bool valid_incl_pow = CheckBlock(block, validation_state_pow, consensus_params, /* fCheckPOW= */ true, /* fCheckMerkleRoot= */ false);
    assert(validation_state_pow.IsValid() || validation_state_pow.IsInvalid() || validation_state_pow.IsError());
    BlockValidationState validation_state_merkle;
    const bool valid_incl_merkle = CheckBlock(block, validation_state_merkle, consensus_params, /* fCheckPOW= */ false, /* fCheckMerkleRoot= */ true);
    assert(validation_state_merkle.IsValid() || validation_state_merkle.IsInvalid() || validation_state_merkle.IsError());
    BlockValidationState validation_state_none;
    const bool valid_incl_none = CheckBlock(block, validation_state_none, consensus_params, /* fCheckPOW= */ false, /* fCheckMerkleRoot= */ false);
    assert(validation_state_none.IsValid() || validation_state_none.IsInvalid() || validation_state_none.IsError());
    if (valid_incl_pow_and_merkle) {
        assert(valid_incl_pow && valid_incl_merkle && valid_incl_none);
    } else if (valid_incl_merkle || valid_incl_pow) {
        assert(valid_incl_none);
    }
    (void)block.GetHash();
    (void)block.ToString();
    (void)BlockMerkleRoot(block);
    if (!block.vtx.empty()) {
        (void)BlockWitnessMerkleRoot(block);
    }
    (void)GetBlockWeight(block);
    (void)GetWitnessCommitmentIndex(block);
    const size_t raw_memory_size = RecursiveDynamicUsage(block);
    const size_t raw_memory_size_as_shared_ptr = RecursiveDynamicUsage(std::make_shared<CBlock>(block));
    assert(raw_memory_size_as_shared_ptr > raw_memory_size);
    CBlock block_copy = block;
    block_copy.SetNull();
    const bool is_null = block_copy.IsNull();
    assert(is_null);
}
