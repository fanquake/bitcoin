// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/txospenderindex.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(txospenderindex_tests)

FIXTURE_TEST_CASE(txospenderindex_initial_sync, TestChain100Setup)
{
    // Setup phase:
    // Mine blocks for coinbase maturity, so we can spend some coinbase outputs in the test.
    const CScript& coinbase_script = m_coinbase_txns[0]->vout[0].scriptPubKey;
    for (int i = 0; i < 10; i++) CreateAndProcessBlock({}, coinbase_script);

    // Spend 10 outputs
    std::vector<COutPoint> spent(10);
    std::vector<CMutableTransaction> spender(spent.size());
    for (size_t i = 0; i < spent.size(); i++) {
        // Outpoint
        auto coinbase_tx = m_coinbase_txns[i];
        spent[i] = COutPoint(coinbase_tx->GetHash(), 0);

        // Spending tx
        spender[i].version = 1;
        spender[i].vin.resize(1);
        spender[i].vin[0].prevout.hash = spent[i].hash;
        spender[i].vin[0].prevout.n = spent[i].n;
        spender[i].vout.resize(1);
        spender[i].vout[0].nValue = coinbase_tx->GetValueOut();
        spender[i].vout[0].scriptPubKey = coinbase_script;

        // Sign
        std::vector<unsigned char> vchSig;
        const uint256 hash = SignatureHash(coinbase_script, spender[i], 0, SIGHASH_ALL, 0, SigVersion::BASE);
        REQUIRE(coinbaseKey.Sign(hash, vchSig));
        vchSig.push_back((unsigned char)SIGHASH_ALL);
        spender[i].vin[0].scriptSig << vchSig;
    }

    // Generate and ensure block has been fully processed
    const uint256 tip_hash = CreateAndProcessBlock(spender, coinbase_script).GetHash();
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    CHECK(WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip()->GetBlockHash()) == tip_hash);

    // Now we concluded the setup phase, run index
    TxoSpenderIndex txospenderindex(interfaces::MakeChain(m_node), 1 << 20, true);
    REQUIRE(txospenderindex.Init());
    CHECK(!txospenderindex.BlockUntilSyncedToCurrentChain()); // false when not synced
    CHECK(txospenderindex.GetSummary().best_block_hash != tip_hash);

    // Transaction should not be found in the index before it is synced.
    for (const auto& outpoint : spent) {
        CHECK(!txospenderindex.FindSpender(outpoint).value());
    }

    txospenderindex.Sync();
    CHECK(txospenderindex.GetSummary().best_block_hash == tip_hash);

    for (size_t i = 0; i < spent.size(); i++) {
        const auto tx_spender{txospenderindex.FindSpender(spent[i])};
        REQUIRE(tx_spender.has_value());
        REQUIRE(tx_spender->has_value());
        CHECK((*tx_spender)->tx->GetHash() == spender[i].GetHash());
        CHECK((*tx_spender)->block_hash == tip_hash);
    }

    // Shutdown sequence (c.f. Shutdown() in init.cpp)
    txospenderindex.Stop();
}

TEST_SUITE_END()
