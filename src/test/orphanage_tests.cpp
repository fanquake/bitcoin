// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <consensus/validation.h>
#include <node/txorphanage.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <test/util/common.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <test/util/transaction_utils.h>

#include <array>
#include <cstdint>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(orphanage_tests)

static void MakeNewKeyWithFastRandomContext(CKey& key, FastRandomContext& rand_ctx)
{
    std::vector<unsigned char> keydata;
    keydata = rand_ctx.randbytes(32);
    key.Set(keydata.data(), keydata.data() + keydata.size(), /*fCompressedIn=*/true);
    assert(key.IsValid());
}

// Creates a transaction with 2 outputs. Spends all outpoints. If outpoints is empty, spends a random one.
static CTransactionRef MakeTransactionSpending(const std::vector<COutPoint>& outpoints, FastRandomContext& det_rand)
{
    CKey key;
    MakeNewKeyWithFastRandomContext(key, det_rand);
    CMutableTransaction tx;
    // If no outpoints are given, create a random one.
    if (outpoints.empty()) {
        tx.vin.emplace_back(Txid::FromUint256(det_rand.rand256()), 0);
    } else {
        for (const auto& outpoint : outpoints) {
            tx.vin.emplace_back(outpoint);
        }
    }
    // Ensure txid != wtxid
    tx.vin[0].scriptWitness.stack.push_back({1});
    tx.vout.resize(2);
    tx.vout[0].nValue = CENT;
    tx.vout[0].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));
    tx.vout[1].nValue = 3 * CENT;
    tx.vout[1].scriptPubKey = GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey()));
    return MakeTransactionRef(tx);
}

// Make another (not necessarily valid) tx with the same txid but different wtxid.
static CTransactionRef MakeMutation(const CTransactionRef& ptx)
{
    CMutableTransaction tx(*ptx);
    tx.vin[0].scriptWitness.stack.push_back({5});
    auto mutated_tx = MakeTransactionRef(tx);
    assert(ptx->GetHash() == mutated_tx->GetHash());
    return mutated_tx;
}

static bool EqualTxns(const std::set<CTransactionRef>& set_txns, const std::vector<CTransactionRef>& vec_txns)
{
    if (vec_txns.size() != set_txns.size()) return false;
    for (const auto& tx : vec_txns) {
        if (!set_txns.contains(tx)) return false;
    }
    return true;
}

FIXTURE_TEST_CASE(peer_dos_limits, BasicTestingSetup)
{
    FastRandomContext det_rand{true};

    // Construct transactions to use. They must all be the same size.
    static constexpr unsigned int NUM_TXNS_CREATED = 100;
    static constexpr int64_t TX_SIZE{469};
    static constexpr int64_t TOTAL_SIZE = NUM_TXNS_CREATED * TX_SIZE;

    std::vector<CTransactionRef> txns;
    txns.reserve(NUM_TXNS_CREATED);
    // All transactions are the same size.
    for (unsigned int i{0}; i < NUM_TXNS_CREATED; ++i) {
        auto ptx = MakeTransactionSpending({}, det_rand);
        txns.emplace_back(ptx);
        CHECK(TX_SIZE == GetTransactionWeight(*ptx));
    }

    // Single peer: eviction is triggered if either limit is hit
    {
        // Test announcement limits
        NodeId peer{8};
        auto orphanage_low_ann = node::MakeTxOrphanage(/*max_global_latency_score=*/1, /*reserved_peer_usage=*/TX_SIZE * 10);
        auto orphanage_low_mem = node::MakeTxOrphanage(/*max_global_latency_score=*/10, /*reserved_peer_usage=*/TX_SIZE);

        // Add the first transaction
        orphanage_low_ann->AddTx(txns.at(0), peer);
        orphanage_low_mem->AddTx(txns.at(0), peer);

        // Add more. One of the limits is exceeded, so LimitOrphans evicts 1.
        orphanage_low_ann->AddTx(txns.at(1), peer);
        orphanage_low_mem->AddTx(txns.at(1), peer);

        // The older transaction is evicted.
        CHECK(!orphanage_low_ann->HaveTx(txns.at(0)->GetWitnessHash()));
        CHECK(!orphanage_low_mem->HaveTx(txns.at(0)->GetWitnessHash()));
        CHECK(orphanage_low_ann->HaveTx(txns.at(1)->GetWitnessHash()));
        CHECK(orphanage_low_mem->HaveTx(txns.at(1)->GetWitnessHash()));

        orphanage_low_ann->SanityCheck();
        orphanage_low_mem->SanityCheck();
    }

    // Single peer: latency score includes inputs
    {
        // Test latency score limits
        NodeId peer{10};
        auto orphanage_low_ann = node::MakeTxOrphanage(/*max_global_latency_score=*/5, /*reserved_peer_usage=*/TX_SIZE * 1000);

        // Add the first transaction
        orphanage_low_ann->AddTx(txns.at(0), peer);

        // Add 1 more transaction with 45 inputs. Even though there are only 2 announcements, this pushes the orphanage above its maximum latency score.
        std::vector<COutPoint> outpoints_45;
        for (unsigned int j{0}; j < 45; ++j) {
            outpoints_45.emplace_back(Txid::FromUint256(det_rand.rand256()), j);
        }
        auto ptx = MakeTransactionSpending(outpoints_45, det_rand);
        orphanage_low_ann->AddTx(ptx, peer);

        // The older transaction is evicted.
        CHECK(!orphanage_low_ann->HaveTx(txns.at(0)->GetWitnessHash()));
        CHECK(orphanage_low_ann->HaveTx(ptx->GetWitnessHash()));

        orphanage_low_ann->SanityCheck();
    }

    // Single peer: eviction order is FIFO on non-reconsiderable, then reconsiderable orphans.
    {
        // Construct parent + child pairs
        std::vector<CTransactionRef> parents;
        std::vector<CTransactionRef> children;
        for (unsigned int i{0}; i < 10; ++i) {
            CTransactionRef parent = MakeTransactionSpending({}, det_rand);
            CTransactionRef child = MakeTransactionSpending({{parent->GetHash(), 0}}, det_rand);
            parents.emplace_back(parent);
            children.emplace_back(child);
        }

        // Test announcement limits
        NodeId peer{9};
        auto orphanage = node::MakeTxOrphanage(/*max_global_latency_score=*/3, /*reserved_peer_usage=*/TX_SIZE * 10);

        // First add a tx which will be made reconsiderable.
        orphanage->AddTx(children.at(0), peer);

        // Then add 2 more orphans... not oversize yet.
        orphanage->AddTx(children.at(1), peer);
        orphanage->AddTx(children.at(2), peer);

        // Make child0 ready to reconsider
        const std::vector<std::pair<Wtxid, NodeId>> expected_set_c0{std::make_pair(children.at(0)->GetWitnessHash(), peer)};
        CHECK(orphanage->AddChildrenToWorkSet(*parents.at(0), det_rand) == expected_set_c0);
        CHECK(orphanage->HaveTxToReconsider(peer));

        // Add 1 more orphan, causing the orphanage to be oversize. child1 is evicted.
        orphanage->AddTx(children.at(3), peer);
        CHECK(orphanage->HaveTx(children.at(0)->GetWitnessHash()));
        CHECK(!orphanage->HaveTx(children.at(1)->GetWitnessHash()));
        CHECK(orphanage->HaveTx(children.at(2)->GetWitnessHash()));
        CHECK(orphanage->HaveTx(children.at(3)->GetWitnessHash()));
        orphanage->SanityCheck();

        // Add 1 more... child2 is evicted.
        orphanage->AddTx(children.at(4), peer);
        CHECK(orphanage->HaveTx(children.at(0)->GetWitnessHash()));
        CHECK(!orphanage->HaveTx(children.at(2)->GetWitnessHash()));
        CHECK(orphanage->HaveTx(children.at(3)->GetWitnessHash()));
        CHECK(orphanage->HaveTx(children.at(4)->GetWitnessHash()));

        // Eviction order is FIFO within the orphans that are read
        const std::vector<std::pair<Wtxid, NodeId>> expected_set_c4{std::make_pair(children.at(4)->GetWitnessHash(), peer)};
        CHECK(orphanage->AddChildrenToWorkSet(*parents.at(4), det_rand) == expected_set_c4);
        const std::vector<std::pair<Wtxid, NodeId>> expected_set_c3{std::make_pair(children.at(3)->GetWitnessHash(), peer)};
        CHECK(orphanage->AddChildrenToWorkSet(*parents.at(3), det_rand) == expected_set_c3);

        // child5 is evicted immediately because it is the only non-reconsiderable orphan.
        orphanage->AddTx(children.at(5), peer);
        CHECK(orphanage->HaveTx(children.at(0)->GetWitnessHash()));
        CHECK(orphanage->HaveTx(children.at(3)->GetWitnessHash()));
        CHECK(orphanage->HaveTx(children.at(4)->GetWitnessHash()));
        CHECK(!orphanage->HaveTx(children.at(5)->GetWitnessHash()));

        // Transactions are marked non-reconsiderable again when returned through GetTxToReconsider
        CHECK(orphanage->GetTxToReconsider(peer) == children.at(0));
        orphanage->AddTx(children.at(6), peer);
        CHECK(!orphanage->HaveTx(children.at(0)->GetWitnessHash()));
        CHECK(orphanage->HaveTx(children.at(3)->GetWitnessHash()));
        CHECK(orphanage->HaveTx(children.at(4)->GetWitnessHash()));
        CHECK(orphanage->HaveTx(children.at(6)->GetWitnessHash()));

        // The first transaction returned from GetTxToReconsider is the older one, not the one that was marked for
        // reconsideration earlier.
        CHECK(orphanage->GetTxToReconsider(peer) == children.at(3));
        CHECK(orphanage->GetTxToReconsider(peer) == children.at(4));

        orphanage->SanityCheck();
    }

    // Multiple peers: when limit is exceeded, we choose the DoSiest peer and evict their oldest transaction.
    {
        NodeId peer_dosy{0};
        NodeId peer1{1};
        NodeId peer2{2};

        unsigned int max_announcements = 60;
        // Set a high per-peer reservation so announcement limit is always hit first.
        auto orphanage = node::MakeTxOrphanage(max_announcements, TOTAL_SIZE * 10);

        // No evictions happen before the global limit is reached.
        for (unsigned int i{0}; i < max_announcements; ++i) {
            orphanage->AddTx(txns.at(i), peer_dosy);
        }
        orphanage->SanityCheck();
        CHECK(orphanage->AnnouncementsFromPeer(peer_dosy) == max_announcements);
        CHECK(orphanage->AnnouncementsFromPeer(peer1) == 0U);
        CHECK(orphanage->AnnouncementsFromPeer(peer2) == 0U);

        // Add 10 unique transactions from peer1.
        // LimitOrphans should evict from peer_dosy, because that's the one exceeding announcement limits.
        unsigned int num_from_peer1 = 10;
        for (unsigned int i{0}; i < num_from_peer1; ++i) {
            orphanage->AddTx(txns.at(max_announcements + i), peer1);
            // The announcement limit per peer has halved, but LimitOrphans does not evict beyond what is necessary to
            // bring the total announcements within its global limit.
            CHECK(orphanage->AnnouncementsFromPeer(peer_dosy) > orphanage->MaxPeerLatencyScore());

            CHECK(orphanage->AnnouncementsFromPeer(peer1) == i + 1);
            CHECK(orphanage->AnnouncementsFromPeer(peer_dosy) == max_announcements - i - 1);

            // Evictions are FIFO within a peer, so the ith transaction sent by peer_dosy is the one that was evicted.
            CHECK(!orphanage->HaveTx(txns.at(i)->GetWitnessHash()));
        }
        // Add 10 transactions that are duplicates of the ones sent by peer_dosy. We need to add 10 because the first 10
        // were just evicted in the previous block additions.
        for (unsigned int i{num_from_peer1}; i < num_from_peer1 + 10; ++i) {
            // Tx has already been sent by peer_dosy
            CHECK(orphanage->HaveTxFromPeer(txns.at(i)->GetWitnessHash(), peer_dosy));
            orphanage->AddTx(txns.at(i), peer2);

            // peer_dosy is still the only one getting evicted
            CHECK(orphanage->AnnouncementsFromPeer(peer_dosy) == max_announcements - i - 1);
            CHECK(orphanage->AnnouncementsFromPeer(peer1) == num_from_peer1);
            CHECK(orphanage->AnnouncementsFromPeer(peer2) == i + 1 - num_from_peer1);

            // Evictions are FIFO within a peer, so the ith transaction sent by peer_dosy is the one that was evicted.
            CHECK(!orphanage->HaveTxFromPeer(txns.at(i)->GetWitnessHash(), peer_dosy));
            CHECK(orphanage->HaveTx(txns.at(i)->GetWitnessHash()));
        }

        // With 6 peers, each can add 10, and still only peer_dosy's orphans are evicted.
        const unsigned int max_per_peer{max_announcements / 6};
        const unsigned int num_announcements{orphanage->CountAnnouncements()};
        for (NodeId peer{3}; peer < 6; ++peer) {
            for (unsigned int i{0}; i < max_per_peer; ++i) {
                // Each addition causes 1 eviction.
                orphanage->AddTx(txns.at(peer * max_per_peer + i), peer);
                CHECK(orphanage->CountAnnouncements() == num_announcements);
            }
        }
        for (NodeId peer{0}; peer < 6; ++peer) {
            CHECK(orphanage->AnnouncementsFromPeer(peer) == max_per_peer);
        }
        orphanage->SanityCheck();
    }

    // Limits change as more peers are added.
    {
        auto orphanage{node::MakeTxOrphanage()};
        // These stay the same regardless of number of peers
        CHECK(orphanage->MaxGlobalLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);
        CHECK(orphanage->ReservedPeerUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);

        // These change with number of peers
        CHECK(orphanage->MaxGlobalUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);
        CHECK(orphanage->MaxPeerLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);

        // Number of peers = 1
        orphanage->AddTx(txns.at(0), 0);
        CHECK(orphanage->MaxGlobalLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);
        CHECK(orphanage->ReservedPeerUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);
        CHECK(orphanage->MaxGlobalUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);
        CHECK(orphanage->MaxPeerLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);

        // Number of peers = 2
        orphanage->AddTx(txns.at(1), 1);
        CHECK(orphanage->MaxGlobalLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);
        CHECK(orphanage->ReservedPeerUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);
        CHECK(orphanage->MaxGlobalUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER * 2);
        CHECK(orphanage->MaxPeerLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE / 2);

        // Number of peers = 3
        orphanage->AddTx(txns.at(2), 2);
        CHECK(orphanage->MaxGlobalLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);
        CHECK(orphanage->ReservedPeerUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);
        CHECK(orphanage->MaxGlobalUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER * 3);
        CHECK(orphanage->MaxPeerLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE / 3);

        // Number of peers didn't change.
        orphanage->AddTx(txns.at(3), 2);
        CHECK(orphanage->MaxGlobalLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);
        CHECK(orphanage->ReservedPeerUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);
        CHECK(orphanage->MaxGlobalUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER * 3);
        CHECK(orphanage->MaxPeerLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE / 3);

        // Once a peer has no orphans, it is not considered in the limits.
        // Number of peers = 2
        orphanage->EraseForPeer(2);
        CHECK(orphanage->MaxGlobalLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);
        CHECK(orphanage->ReservedPeerUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);
        CHECK(orphanage->MaxGlobalUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER * 2);
        CHECK(orphanage->MaxPeerLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE / 2);

        // Number of peers = 1
        orphanage->EraseTx(txns.at(0)->GetWitnessHash());
        CHECK(orphanage->MaxGlobalLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);
        CHECK(orphanage->ReservedPeerUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);
        CHECK(orphanage->MaxGlobalUsage() == node::DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER);
        CHECK(orphanage->MaxPeerLatencyScore() == node::DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE);

        orphanage->SanityCheck();
    }

    // Test eviction of multiple transactions at a time
    {
        // Create a large transaction that is 10 times larger than the normal size transaction.
        CMutableTransaction tx_large;
        tx_large.vin.resize(1);
        BulkTransaction(tx_large, 10 * TX_SIZE);
        auto ptx_large = MakeTransactionRef(tx_large);

        const auto large_tx_size = GetTransactionWeight(*ptx_large);
        CHECK(large_tx_size > 10 * TX_SIZE);
        CHECK(large_tx_size < 11 * TX_SIZE);

        auto orphanage = node::MakeTxOrphanage(20, large_tx_size);
        // One peer sends 10 normal size transactions. The other peer sends 10 normal transactions and 1 very large one
        NodeId peer_normal{0};
        NodeId peer_large{1};
        for (unsigned int i = 0; i < 20; i++) {
            orphanage->AddTx(txns.at(i), i < 10 ? peer_normal : peer_large);
        }
        CHECK(orphanage->TotalLatencyScore() <= orphanage->MaxGlobalLatencyScore());
        CHECK(orphanage->TotalOrphanUsage() <= orphanage->MaxGlobalUsage());

        // Add the large transaction. This should cause evictions of all the previous 10 transactions from that peer.
        orphanage->AddTx(ptx_large, peer_large);

        // peer_normal should still have 10 transactions, and peer_large should have 1.
        CHECK(orphanage->AnnouncementsFromPeer(peer_normal) == 10U);
        CHECK(orphanage->AnnouncementsFromPeer(peer_large) == 1U);
        CHECK(orphanage->HaveTxFromPeer(ptx_large->GetWitnessHash(), peer_large));
        CHECK(orphanage->CountAnnouncements() == 11U);

        orphanage->SanityCheck();
    }

    // Test that latency score includes number of inputs.
    {
        auto orphanage = node::MakeTxOrphanage();

        // Add 10 transactions with 9 inputs each.
        std::vector<COutPoint> outpoints_9;
        for (unsigned int j{0}; j < 9; ++j) {
            outpoints_9.emplace_back(Txid::FromUint256(m_rng.rand256()), j);
        }
        for (unsigned int i{0}; i < 10; ++i) {
            auto ptx = MakeTransactionSpending(outpoints_9, m_rng);
            orphanage->AddTx(ptx, 0);
        }
        CHECK(orphanage->CountAnnouncements() == 10U);
        CHECK(orphanage->TotalLatencyScore() == 10U);

        // Add 10 transactions with 50 inputs each.
        std::vector<COutPoint> outpoints_50;
        for (unsigned int j{0}; j < 50; ++j) {
            outpoints_50.emplace_back(Txid::FromUint256(m_rng.rand256()), j);
        }

        for (unsigned int i{0}; i < 10; ++i) {
            CMutableTransaction tx;
            std::shuffle(outpoints_50.begin(), outpoints_50.end(), m_rng);
            auto ptx = MakeTransactionSpending(outpoints_50, m_rng);
            CHECK(orphanage->AddTx(ptx, 0));
            if (i < 5) CHECK(!orphanage->AddTx(ptx, 1));
        }
        // 10 of the 9-input transactions + 10 of the 50-input transactions + 5 more announcements of the 50-input transactions
        CHECK(orphanage->CountAnnouncements() == 25U);
        // Base of 25 announcements, plus 10 * 5 for the 50-input transactions (counted just once)
        CHECK(orphanage->TotalLatencyScore() == 25U + 50U);

        // Peer 0 sent all 20 transactions
        CHECK(orphanage->AnnouncementsFromPeer(0) == 20U);
        CHECK(orphanage->LatencyScoreFromPeer(0) == 20U + 10U * 5U);

        // Peer 1 sent 5 of the 10 transactions with many inputs
        CHECK(orphanage->AnnouncementsFromPeer(1) == 5U);
        CHECK(orphanage->LatencyScoreFromPeer(1) == 5U + 5U * 5U);

        orphanage->SanityCheck();
    }
}
FIXTURE_TEST_CASE(DoS_mapOrphans, BasicTestingSetup)
{
    // This test had non-deterministic coverage due to
    // randomly selected seeds.
    // This seed is chosen so that all branches of the function
    // ecdsa_signature_parse_der_lax are executed during this test.
    // Specifically branches that run only when an ECDSA
    // signature's R and S values have leading zeros.
    m_rng.Reseed(uint256{33});

    std::unique_ptr<node::TxOrphanage> orphanage{node::MakeTxOrphanage()};
    CKey key;
    MakeNewKeyWithFastRandomContext(key, m_rng);
    FillableSigningProvider keystore;
    CHECK(keystore.AddKey(key));

    FakeNodeClock clock{};

    std::vector<CTransactionRef> orphans_added;

    // 50 orphan transactions:
    for (int i = 0; i < 50; i++)
    {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].prevout.hash = Txid::FromUint256(m_rng.rand256());
        tx.vin[0].scriptSig << OP_1;
        tx.vout.resize(1);
        tx.vout[0].nValue = i*CENT;
        tx.vout[0].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));

        auto ptx = MakeTransactionRef(tx);
        orphanage->AddTx(ptx, i);
        orphans_added.emplace_back(ptx);
    }

    // ... and 50 that depend on other orphans:
    for (int i = 0; i < 50; i++)
    {
        const auto& txPrev = orphans_added[m_rng.randrange(orphans_added.size())];

        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].prevout.hash = txPrev->GetHash();
        tx.vout.resize(1);
        tx.vout[0].nValue = i*CENT;
        tx.vout[0].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));
        SignatureData empty;
        CHECK(SignSignature(keystore, *txPrev, tx, 0, SIGHASH_ALL, empty));

        auto ptx = MakeTransactionRef(tx);
        orphanage->AddTx(ptx, i);
        orphans_added.emplace_back(ptx);
    }

    // This really-big orphan should be ignored:
    for (int i = 0; i < 10; i++)
    {
        const auto& txPrev = orphans_added[m_rng.randrange(orphans_added.size())];

        CMutableTransaction tx;
        tx.vout.resize(1);
        tx.vout[0].nValue = 1*CENT;
        tx.vout[0].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));
        tx.vin.resize(2777);
        for (unsigned int j = 0; j < tx.vin.size(); j++)
        {
            tx.vin[j].prevout.n = j;
            tx.vin[j].prevout.hash = txPrev->GetHash();
        }
        SignatureData empty;
        CHECK(SignSignature(keystore, *txPrev, tx, 0, SIGHASH_ALL, empty));
        // Reuse same signature for other inputs
        // (they don't have to be valid for this test)
        for (unsigned int j = 1; j < tx.vin.size(); j++)
            tx.vin[j].scriptSig = tx.vin[0].scriptSig;

        CHECK(!orphanage->AddTx(MakeTransactionRef(tx), i));
    }

    size_t expected_num_orphans = orphanage->CountUniqueOrphans();

    // Non-existent peer; nothing should be deleted
    orphanage->EraseForPeer(/*peer=*/-1);
    CHECK(orphanage->CountUniqueOrphans() == expected_num_orphans);

    // Each of first three peers stored
    // two transactions each.
    for (NodeId i = 0; i < 3; i++)
    {
        orphanage->EraseForPeer(i);
        expected_num_orphans -= 2;
        CHECK(orphanage->CountUniqueOrphans() == expected_num_orphans);
    }
}

FIXTURE_TEST_CASE(same_txid_diff_witness, BasicTestingSetup)
{
    FastRandomContext det_rand{true};
    std::unique_ptr<node::TxOrphanage> orphanage{node::MakeTxOrphanage()};
    NodeId peer{0};

    std::vector<COutPoint> empty_outpoints;
    auto parent = MakeTransactionSpending(empty_outpoints, det_rand);

    // Create children to go into orphanage.
    auto child_normal = MakeTransactionSpending({{parent->GetHash(), 0}}, det_rand);
    auto child_mutated = MakeMutation(child_normal);

    const auto& normal_wtxid = child_normal->GetWitnessHash();
    const auto& mutated_wtxid = child_mutated->GetWitnessHash();
    CHECK(normal_wtxid != mutated_wtxid);

    CHECK(orphanage->AddTx(child_normal, peer));
    // EraseTx fails as transaction by this wtxid doesn't exist.
    CHECK(orphanage->EraseTx(mutated_wtxid) == 0);
    CHECK(orphanage->HaveTx(normal_wtxid));
    CHECK(orphanage->GetTx(normal_wtxid) == child_normal);
    CHECK(!orphanage->HaveTx(mutated_wtxid));
    CHECK(orphanage->GetTx(mutated_wtxid) == nullptr);

    // Must succeed. Both transactions should be present in orphanage.
    CHECK(orphanage->AddTx(child_mutated, peer));
    CHECK(orphanage->HaveTx(normal_wtxid));
    CHECK(orphanage->HaveTx(mutated_wtxid));

    // Outpoints map should track all entries: check that both are returned as children of the parent.
    std::set<CTransactionRef> expected_children{child_normal, child_mutated};
    CHECK(EqualTxns(expected_children, orphanage->GetChildrenFromSamePeer(parent, peer)));

    // Erase by wtxid: mutated first
    CHECK(orphanage->EraseTx(mutated_wtxid) == 1);
    CHECK(orphanage->HaveTx(normal_wtxid));
    CHECK(!orphanage->HaveTx(mutated_wtxid));

    CHECK(orphanage->EraseTx(normal_wtxid) == 1);
    CHECK(!orphanage->HaveTx(normal_wtxid));
    CHECK(!orphanage->HaveTx(mutated_wtxid));
}


FIXTURE_TEST_CASE(get_children, BasicTestingSetup)
{
    FastRandomContext det_rand{true};
    std::vector<COutPoint> empty_outpoints;

    auto parent1 = MakeTransactionSpending(empty_outpoints, det_rand);
    auto parent2 = MakeTransactionSpending(empty_outpoints, det_rand);

    // Make sure these parents have different txids otherwise this test won't make sense.
    while (parent1->GetHash() == parent2->GetHash()) {
        parent2 = MakeTransactionSpending(empty_outpoints, det_rand);
    }

    // Create children to go into orphanage.
    auto child_p1n0 = MakeTransactionSpending({{parent1->GetHash(), 0}}, det_rand);
    auto child_p2n1 = MakeTransactionSpending({{parent2->GetHash(), 1}}, det_rand);
    // Spends the same tx twice. Should not cause duplicates.
    auto child_p1n0_p1n1 = MakeTransactionSpending({{parent1->GetHash(), 0}, {parent1->GetHash(), 1}}, det_rand);
    // Spends the same outpoint as previous tx. Should still be returned; don't assume outpoints are unique.
    auto child_p1n0_p2n0 = MakeTransactionSpending({{parent1->GetHash(), 0}, {parent2->GetHash(), 0}}, det_rand);

    const NodeId node0{0};
    const NodeId node1{1};
    const NodeId node2{2};
    const NodeId node3{3};

    // All orphans provided by node1
    {
        auto orphanage{node::MakeTxOrphanage()};
        CHECK(orphanage->AddTx(child_p1n0, node1));
        CHECK(orphanage->AddTx(child_p2n1, node1));
        CHECK(orphanage->AddTx(child_p1n0_p1n1, node1));
        CHECK(orphanage->AddTx(child_p1n0_p2n0, node1));

        // Also add some other announcers for the same transactions
        CHECK(!orphanage->AddTx(child_p1n0_p1n1, node0));
        CHECK(!orphanage->AddTx(child_p2n1, node0));
        CHECK(!orphanage->AddTx(child_p1n0, node3));


        std::vector<CTransactionRef> expected_parent1_children{child_p1n0_p2n0, child_p1n0_p1n1, child_p1n0};
        std::vector<CTransactionRef> expected_parent2_children{child_p1n0_p2n0, child_p2n1};

        CHECK(expected_parent1_children == orphanage->GetChildrenFromSamePeer(parent1, node1));
        CHECK(expected_parent2_children == orphanage->GetChildrenFromSamePeer(parent2, node1));

        // The peer must match
        CHECK(orphanage->GetChildrenFromSamePeer(parent1, node2).empty());
        CHECK(orphanage->GetChildrenFromSamePeer(parent2, node2).empty());

        // There shouldn't be any children of this tx in the orphanage
        CHECK(orphanage->GetChildrenFromSamePeer(child_p1n0_p2n0, node1).empty());
        CHECK(orphanage->GetChildrenFromSamePeer(child_p1n0_p2n0, node2).empty());
    }

    // Orphans provided by node1 and node2
    {
        std::unique_ptr<node::TxOrphanage> orphanage{node::MakeTxOrphanage()};
        CHECK(orphanage->AddTx(child_p1n0, node1));
        CHECK(orphanage->AddTx(child_p2n1, node1));
        CHECK(orphanage->AddTx(child_p1n0_p1n1, node2));
        CHECK(orphanage->AddTx(child_p1n0_p2n0, node2));

        // +----------------+---------------+----------------------------------+
        // |                | sender=node1  |           sender=node2           |
        // +----------------+---------------+----------------------------------+
        // | spends parent1 | child_p1n0    | child_p1n0_p1n1, child_p1n0_p2n0 |
        // | spends parent2 | child_p2n1    | child_p1n0_p2n0                  |
        // +----------------+---------------+----------------------------------+

        // Children of parent1 from node1:
        {
            std::set<CTransactionRef> expected_parent1_node1{child_p1n0};

            CHECK(orphanage->GetChildrenFromSamePeer(parent1, node1).size() == 1U);
            CHECK(orphanage->HaveTxFromPeer(child_p1n0->GetWitnessHash(), node1));
            CHECK(EqualTxns(expected_parent1_node1, orphanage->GetChildrenFromSamePeer(parent1, node1)));
        }

        // Children of parent2 from node1:
        {
            std::set<CTransactionRef> expected_parent2_node1{child_p2n1};

            CHECK(EqualTxns(expected_parent2_node1, orphanage->GetChildrenFromSamePeer(parent2, node1)));
        }

        // Children of parent1 from node2: newest returned first.
        {
            std::vector<CTransactionRef> expected_parent1_node2{child_p1n0_p2n0, child_p1n0_p1n1};
            CHECK(orphanage->HaveTxFromPeer(child_p1n0_p1n1->GetWitnessHash(), node2));
            CHECK(orphanage->HaveTxFromPeer(child_p1n0_p2n0->GetWitnessHash(), node2));
            CHECK(expected_parent1_node2 == orphanage->GetChildrenFromSamePeer(parent1, node2));
        }

        // Children of parent2 from node2:
        {
            std::set<CTransactionRef> expected_parent2_node2{child_p1n0_p2n0};

            CHECK(1U == orphanage->GetChildrenFromSamePeer(parent2, node2).size());
            CHECK(orphanage->HaveTxFromPeer(child_p1n0_p2n0->GetWitnessHash(), node2));
            CHECK(EqualTxns(expected_parent2_node2, orphanage->GetChildrenFromSamePeer(parent2, node2)));
        }
    }
}

FIXTURE_TEST_CASE(too_large_orphan_tx, BasicTestingSetup)
{
    std::unique_ptr<node::TxOrphanage> orphanage{node::MakeTxOrphanage()};
    CMutableTransaction tx;
    tx.vin.resize(1);

    // check that txs larger than MAX_STANDARD_TX_WEIGHT are not added to the orphanage
    BulkTransaction(tx, MAX_STANDARD_TX_WEIGHT + 4);
    CHECK(GetTransactionWeight(CTransaction(tx)) == MAX_STANDARD_TX_WEIGHT + 4);
    CHECK(!orphanage->AddTx(MakeTransactionRef(tx), 0));

    tx.vout.clear();
    BulkTransaction(tx, MAX_STANDARD_TX_WEIGHT);
    CHECK(GetTransactionWeight(CTransaction(tx)) == MAX_STANDARD_TX_WEIGHT);
    CHECK(orphanage->AddTx(MakeTransactionRef(tx), 0));
}

FIXTURE_TEST_CASE(process_block, BasicTestingSetup)
{
    FastRandomContext det_rand{true};
    std::unique_ptr<node::TxOrphanage> orphanage{node::MakeTxOrphanage()};

    // Create outpoints that will be spent by transactions in the block
    std::vector<COutPoint> outpoints;
    const uint32_t num_outpoints{6};
    outpoints.reserve(num_outpoints);
    for (uint32_t i{0}; i < num_outpoints; ++i) {
        // All the hashes should be different, but change the n just in case.
        outpoints.emplace_back(Txid::FromUint256(det_rand.rand256()), i);
    }

    CBlock block;
    const NodeId node{0};

    auto control_tx = MakeTransactionSpending({}, det_rand);
    CHECK(orphanage->AddTx(control_tx, node));

    auto bo_tx_same_txid = MakeTransactionSpending({outpoints.at(0)}, det_rand);
    CHECK(orphanage->AddTx(bo_tx_same_txid, node));
    block.vtx.emplace_back(bo_tx_same_txid);

    // 2 transactions with the same txid but different witness
    auto b_tx_same_txid_diff_witness = MakeTransactionSpending({outpoints.at(1)}, det_rand);
    block.vtx.emplace_back(b_tx_same_txid_diff_witness);

    auto o_tx_same_txid_diff_witness = MakeMutation(b_tx_same_txid_diff_witness);
    CHECK(orphanage->AddTx(o_tx_same_txid_diff_witness, node));

    // 2 different transactions that spend the same input.
    auto b_tx_conflict = MakeTransactionSpending({outpoints.at(2)}, det_rand);
    block.vtx.emplace_back(b_tx_conflict);

    auto o_tx_conflict = MakeTransactionSpending({outpoints.at(2)}, det_rand);
    CHECK(orphanage->AddTx(o_tx_conflict, node));

    // 2 different transactions that have 1 overlapping input.
    auto b_tx_conflict_partial = MakeTransactionSpending({outpoints.at(3), outpoints.at(4)}, det_rand);
    block.vtx.emplace_back(b_tx_conflict_partial);

    auto o_tx_conflict_partial_2 = MakeTransactionSpending({outpoints.at(4), outpoints.at(5)}, det_rand);
    CHECK(orphanage->AddTx(o_tx_conflict_partial_2, node));

    orphanage->EraseForBlock(block);
    for (const auto& expected_removed : {bo_tx_same_txid, o_tx_same_txid_diff_witness, o_tx_conflict, o_tx_conflict_partial_2}) {
        const auto& expected_removed_wtxid = expected_removed->GetWitnessHash();
        CHECK(!orphanage->HaveTx(expected_removed_wtxid));
    }
    // Only remaining tx is control_tx
    CHECK(orphanage->CountUniqueOrphans() == 1U);
    CHECK(orphanage->HaveTx(control_tx->GetWitnessHash()));
}

FIXTURE_TEST_CASE(multiple_announcers, BasicTestingSetup)
{
    const NodeId node0{0};
    const NodeId node1{1};
    const NodeId node2{2};
    size_t expected_total_count{0};
    FastRandomContext det_rand{true};
    std::unique_ptr<node::TxOrphanage> orphanage{node::MakeTxOrphanage()};

    // Check accounting per peer.
    // Check that EraseForPeer works with multiple announcers.
    {
        auto ptx = MakeTransactionSpending({}, det_rand);
        const auto& wtxid = ptx->GetWitnessHash();
        CHECK(orphanage->AddTx(ptx, node0));
        CHECK(orphanage->HaveTx(wtxid));
        expected_total_count += 1;
        CHECK(orphanage->CountUniqueOrphans() == expected_total_count);

        // Adding again should do nothing.
        CHECK(!orphanage->AddTx(ptx, node0));
        CHECK(orphanage->CountUniqueOrphans() == expected_total_count);

        // We can add another tx with the same txid but different witness.
        auto ptx_mutated{MakeMutation(ptx)};
        CHECK(orphanage->AddTx(ptx_mutated, node0));
        CHECK(orphanage->HaveTx(ptx_mutated->GetWitnessHash()));
        expected_total_count += 1;

        CHECK(!orphanage->AddTx(ptx, node0));

        // Adding a new announcer should not change overall accounting.
        CHECK(orphanage->AddAnnouncer(ptx->GetWitnessHash(), node2));
        CHECK(orphanage->CountUniqueOrphans() == expected_total_count);

        // If we already have this announcer, AddAnnouncer returns false.
        CHECK(orphanage->HaveTxFromPeer(ptx->GetWitnessHash(), node2));
        CHECK(!orphanage->AddAnnouncer(ptx->GetWitnessHash(), node2));

        // Same with using AddTx for an existing tx, which is equivalent to using AddAnnouncer
        CHECK(!orphanage->AddTx(ptx, node1));
        CHECK(orphanage->CountUniqueOrphans() == expected_total_count);

        // if EraseForPeer is called for an orphan with multiple announcers, the orphanage should only
        // erase that peer from the announcers set.
        orphanage->EraseForPeer(node0);
        CHECK(orphanage->HaveTx(ptx->GetWitnessHash()));
        CHECK(!orphanage->HaveTxFromPeer(ptx->GetWitnessHash(), node0));
        // node0 is the only one that announced ptx_mutated
        CHECK(!orphanage->HaveTx(ptx_mutated->GetWitnessHash()));
        expected_total_count -= 1;
        CHECK(orphanage->CountUniqueOrphans() == expected_total_count);

        // EraseForPeer should delete the orphan if it's the only announcer left.
        orphanage->EraseForPeer(node1);
        CHECK(orphanage->CountUniqueOrphans() == expected_total_count);
        CHECK(orphanage->HaveTx(ptx->GetWitnessHash()));
        orphanage->EraseForPeer(node2);
        expected_total_count -= 1;
        CHECK(orphanage->CountUniqueOrphans() == expected_total_count);
        CHECK(!orphanage->HaveTx(ptx->GetWitnessHash()));
    }

    // Check that erasure for blocks removes for all peers.
    {
        CBlock block;
        auto tx_block = MakeTransactionSpending({}, det_rand);
        block.vtx.emplace_back(tx_block);
        CHECK(orphanage->AddTx(tx_block, node0));
        CHECK(!orphanage->AddTx(tx_block, node1));

        expected_total_count += 1;

        CHECK(orphanage->CountUniqueOrphans() == expected_total_count);

        orphanage->EraseForBlock(block);

        expected_total_count -= 1;

        CHECK(orphanage->CountUniqueOrphans() == expected_total_count);
    }
}
FIXTURE_TEST_CASE(peer_worksets, BasicTestingSetup)
{
    const NodeId node0{0};
    const NodeId node1{1};
    const NodeId node2{2};
    FastRandomContext det_rand{true};
    std::unique_ptr<node::TxOrphanage> orphanage{node::MakeTxOrphanage()};
    // AddChildrenToWorkSet should pick an announcer randomly
    {
        auto tx_missing_parent = MakeTransactionSpending({}, det_rand);
        auto tx_orphan = MakeTransactionSpending({COutPoint{tx_missing_parent->GetHash(), 0}}, det_rand);
        const auto& orphan_wtxid = tx_orphan->GetWitnessHash();

        // All 3 peers are announcers.
        CHECK(orphanage->AddTx(tx_orphan, node0));
        CHECK(!orphanage->AddTx(tx_orphan, node1));
        CHECK(orphanage->AddAnnouncer(orphan_wtxid, node2));
        for (NodeId node = node0; node <= node2; ++node) {
            CHECK(orphanage->HaveTxFromPeer(orphan_wtxid, node));
        }

        // Parent accepted: child is added to 1 of 3 worksets.
        auto newly_reconsiderable = orphanage->AddChildrenToWorkSet(*tx_missing_parent, det_rand);
        CHECK(newly_reconsiderable.size() == 1U);
        int node0_reconsider = orphanage->HaveTxToReconsider(node0);
        int node1_reconsider = orphanage->HaveTxToReconsider(node1);
        int node2_reconsider = orphanage->HaveTxToReconsider(node2);
        CHECK(node0_reconsider + node1_reconsider + node2_reconsider == 1);

        NodeId assigned_peer;
        if (node0_reconsider) {
            assigned_peer = node0;
        } else if (node1_reconsider) {
            assigned_peer = node1;
        } else {
            CHECK(node2_reconsider);
            assigned_peer = node2;
        }

        // EraseForPeer also removes that tx from the workset.
        orphanage->EraseForPeer(assigned_peer);
        CHECK(orphanage->GetTxToReconsider(node0) == nullptr);

        // Delete this tx, clearing the orphanage.
        CHECK(orphanage->EraseTx(orphan_wtxid) == 1);
        CHECK(orphanage->CountUniqueOrphans() == 0U);
        for (NodeId node = node0; node <= node2; ++node) {
            CHECK(orphanage->GetTxToReconsider(node) == nullptr);
            CHECK(!orphanage->HaveTxFromPeer(orphan_wtxid, node));
        }
    }
}
TEST_SUITE_END()
