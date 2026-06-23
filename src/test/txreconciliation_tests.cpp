// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/txreconciliation.h>

#include <test/util/common.h>
#include <test/util/setup_common.h>

#include <test/util/framework.h>

TEST_SUITE_BEGIN(txreconciliation_tests)

FIXTURE_TEST_CASE(RegisterPeerTest, BasicTestingSetup)
{
    TxReconciliationTracker tracker(TXRECONCILIATION_VERSION);
    const uint64_t salt = 0;

    // Prepare a peer for reconciliation.
    tracker.PreRegisterPeer(0);

    // Invalid version.
    CHECK(tracker.RegisterPeer(/*peer_id=*/0, /*is_peer_inbound=*/true,
                                           /*peer_recon_version=*/0, salt) == ReconciliationRegisterResult::PROTOCOL_VIOLATION);

    // Valid registration (inbound and outbound peers).
    REQUIRE(!tracker.IsPeerRegistered(0));
    REQUIRE(tracker.RegisterPeer(0, true, 1, salt) == ReconciliationRegisterResult::SUCCESS);
    CHECK(tracker.IsPeerRegistered(0));
    REQUIRE(!tracker.IsPeerRegistered(1));
    tracker.PreRegisterPeer(1);
    REQUIRE(tracker.RegisterPeer(1, false, 1, salt) == ReconciliationRegisterResult::SUCCESS);
    CHECK(tracker.IsPeerRegistered(1));

    // Reconciliation version is higher than ours, should be able to register.
    REQUIRE(!tracker.IsPeerRegistered(2));
    tracker.PreRegisterPeer(2);
    REQUIRE(tracker.RegisterPeer(2, true, 2, salt) == ReconciliationRegisterResult::SUCCESS);
    CHECK(tracker.IsPeerRegistered(2));

    // Try registering for the second time.
    REQUIRE(tracker.RegisterPeer(1, false, 1, salt) == ReconciliationRegisterResult::ALREADY_REGISTERED);

    // Do not register if there were no pre-registration for the peer.
    REQUIRE(tracker.RegisterPeer(100, true, 1, salt) == ReconciliationRegisterResult::NOT_FOUND);
    CHECK(!tracker.IsPeerRegistered(100));
}

FIXTURE_TEST_CASE(ForgetPeerTest, BasicTestingSetup)
{
    TxReconciliationTracker tracker(TXRECONCILIATION_VERSION);
    NodeId peer_id0 = 0;

    // Removing peer after pre-registering works and does not let to register the peer.
    tracker.PreRegisterPeer(peer_id0);
    tracker.ForgetPeer(peer_id0);
    CHECK(tracker.RegisterPeer(peer_id0, true, 1, 1) == ReconciliationRegisterResult::NOT_FOUND);

    // Removing peer after it is registered works.
    tracker.PreRegisterPeer(peer_id0);
    REQUIRE(!tracker.IsPeerRegistered(peer_id0));
    REQUIRE(tracker.RegisterPeer(peer_id0, true, 1, 1) == ReconciliationRegisterResult::SUCCESS);
    CHECK(tracker.IsPeerRegistered(peer_id0));
    tracker.ForgetPeer(peer_id0);
    CHECK(!tracker.IsPeerRegistered(peer_id0));
}

FIXTURE_TEST_CASE(IsPeerRegisteredTest, BasicTestingSetup)
{
    TxReconciliationTracker tracker(TXRECONCILIATION_VERSION);
    NodeId peer_id0 = 0;

    REQUIRE(!tracker.IsPeerRegistered(peer_id0));
    tracker.PreRegisterPeer(peer_id0);
    REQUIRE(!tracker.IsPeerRegistered(peer_id0));

    REQUIRE(tracker.RegisterPeer(peer_id0, true, 1, 1) == ReconciliationRegisterResult::SUCCESS);
    CHECK(tracker.IsPeerRegistered(peer_id0));

    tracker.ForgetPeer(peer_id0);
    CHECK(!tracker.IsPeerRegistered(peer_id0));
}

TEST_SUITE_END()
