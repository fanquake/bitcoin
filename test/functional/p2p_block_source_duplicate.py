#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a second copy of a stored, not yet connected block does not clear who sent the first.

Peer A sends an invalid block (it spends an output that does not exist, which only connecting it shows)
before its parent, so the block is stored and waits. Peer B sends the same block. A then sends the parent,
the invalid block is connected and fails, and A, its first sender, is disconnected for it.
"""
from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    msg_block,
    msg_headers,
    CBlockHeader,
)
from test_framework.p2p import P2PInterface
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class BlockSourceDuplicateTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        tip = node.getbestblockhash()
        best = node.getblock(tip)
        height = best["height"] + 1

        parent = create_block(int(tip, 16), create_coinbase(height), ntime=best["time"] + 1)
        parent.solve()
        bad = CTransaction()
        bad.vin = [CTxIn(COutPoint(0x1234, 0), CScript([OP_TRUE]))]
        bad.vout = [CTxOut(1000, CScript([OP_TRUE]))]
        child = create_block(parent.hash_int, create_coinbase(height + 1), ntime=best["time"] + 2, txlist=[bad])
        child.solve()

        peer_a = node.add_p2p_connection(P2PInterface())
        peer_b = node.add_p2p_connection(P2PInterface())

        self.log.info("A announces both headers and sends the invalid child first: stored, not connected")
        peer_a.send_and_ping(msg_headers([CBlockHeader(parent), CBlockHeader(child)]))
        peer_a.send_and_ping(msg_block(child))
        assert_equal(node.getblockheader(child.hash_hex)["confirmations"], -1)

        self.log.info("B sends the same block")
        peer_b.send_and_ping(msg_block(child))

        self.log.info("A sends the parent: the child fails, and A is disconnected for it")
        with node.assert_debug_log(expected_msgs=["bad-txns-inputs-missingorspent"]):
            peer_a.send_without_ping(msg_block(parent))
            peer_a.wait_for_disconnect()
        assert_equal(node.getbestblockhash(), parent.hash_hex)
        assert peer_b.is_connected


if __name__ == '__main__':
    BlockSourceDuplicateTest(__file__).main()
