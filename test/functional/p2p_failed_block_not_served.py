#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test that a block announced before it is connected is served only once it is.

A new block at the tip is announced (NewPoWValidBlock) once its own checks pass, before it is written and
connected; a request that comes while it is being written is asked again once it is stored. Once it failed
its connection it is refused, as a block not validated is: a peer that asked for it after a compact block
would punish the sender of an invalid full block. The write is too short for a request to be timed inside it, so
the test checks the outcomes on either side of it.

These outcomes are the ones of a node that connects each block on the message handler: the test passes there
too. Its node is out of initial block download, where a block is written on the message handler here as well
and connected by the connector thread: the test holds the outcomes for that, and does not reach the receive
tasks of the connect pool.
"""

from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CInv,
    MSG_BLOCK,
    MSG_WITNESS_FLAG,
    msg_block,
    msg_getdata,
)
from test_framework.p2p import (
    P2PInterface,
    p2p_lock,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class FailedBlockNotServedTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def next_block(self, coinbase_value=50):
        node = self.nodes[0]
        tip = node.getbestblockhash()
        height = node.getblockcount() + 1
        block = create_block(int(tip, 16), create_coinbase(height, nValue=coinbase_value), ntime=node.getblock(tip)["time"] + 1)
        block.solve()
        return block

    def request(self, block):
        """Asks a fresh peer's question for the full block and returns whether the block came."""
        node = self.nodes[0]
        requester = node.add_p2p_connection(P2PInterface())
        requester.send_and_ping(msg_getdata([CInv(MSG_BLOCK | MSG_WITNESS_FLAG, block.hash_int)]))
        with p2p_lock:
            served = "block" in requester.last_message and requester.last_message["block"].block.hash_int == block.hash_int
        requester.peer_disconnect()
        requester.wait_for_disconnect()
        return served

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 1)
        assert not node.getblockchaininfo()["initialblockdownload"]

        self.log.info("A valid new block is announced, connected and served")
        sender = node.add_p2p_connection(P2PInterface())
        valid = self.next_block()
        with node.assert_debug_log(expected_msgs=[f"NewPoWValidBlock: block hash={valid.hash_hex}"]):
            sender.send_and_ping(msg_block(valid))
        assert_equal(node.getbestblockhash(), valid.hash_hex)
        assert self.request(valid)

        self.log.info("A new block that fails its connection is announced, but not served afterwards")
        tip = node.getbestblockhash()
        invalid = self.next_block(coinbase_value=51)
        with node.assert_debug_log(expected_msgs=[f"NewPoWValidBlock: block hash={invalid.hash_hex}", "bad-cb-amount"]):
            sender.send_without_ping(msg_block(invalid))
            sender.wait_for_disconnect()
        assert_equal(node.getbestblockhash(), tip)
        # The block is still the one announced last; it failed, so it is refused.
        assert not self.request(invalid)


if __name__ == '__main__':
    FailedBlockNotServedTest(__file__).main()
