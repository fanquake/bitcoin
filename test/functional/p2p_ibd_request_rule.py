#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test how many blocks a peer is asked for at a time during initial block download.

A peer is asked for one block until its first block arrived, then for one more than it delivers in two
seconds, going by a smoothed time between its blocks, and never for more than sixteen. The clock is mocked, so
a delivery takes exactly the time the test lets pass.
"""

import time

from test_framework.blocktools import (
    TIME_GENESIS_BLOCK,
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlockHeader,
    MSG_BLOCK,
    MSG_TYPE_MASK,
    msg_block,
    msg_headers,
)
from test_framework.p2p import (
    P2PInterface,
    p2p_lock,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16
NUM_BLOCKS = 200


class RequestCounter(P2PInterface):
    """A peer that records the blocks it is asked for and sends them only when told to."""

    def __init__(self):
        super().__init__()
        self.requested = []
        self.delivered = 0

    def on_getdata(self, message):
        for inv in message.inv:
            if (inv.type & MSG_TYPE_MASK) == MSG_BLOCK:
                self.requested.append(inv.hash)

    def outstanding(self):
        with p2p_lock:
            return self.requested[self.delivered:]


class IBDRequestRuleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        self.mocktime = int(time.time())
        node.setmocktime(self.mocktime)

        # Dated just after genesis, so that the node stays in initial block download.
        blocks = {}
        prev_hash = int(node.getbestblockhash(), 16)
        headers = []
        for height in range(1, NUM_BLOCKS + 1):
            block = create_block(prev_hash, create_coinbase(height), ntime=TIME_GENESIS_BLOCK + height)
            block.solve()
            blocks[block.hash_int] = block
            headers.append(CBlockHeader(block))
            prev_hash = block.hash_int

        self.peer = node.add_p2p_connection(RequestCounter())
        self.peer.send_without_ping(msg_headers(headers))
        self.wait_until(lambda: len(self.peer.outstanding()) > 0)

        def deliver_and_count(seconds):
            """Delivers every outstanding block, letting `seconds` pass before each, and returns how many the
            node then asks for at a time."""
            batch = self.peer.outstanding()
            with p2p_lock:
                self.peer.delivered += len(batch)
            for block_hash in batch:
                if seconds:
                    self.mocktime += seconds
                    node.setmocktime(self.mocktime)
                self.peer.send_without_ping(msg_block(blocks[block_hash]))
                if seconds:
                    # The node reads the block before the clock moves again, so that it took exactly `seconds`.
                    self.peer.sync_with_ping()
            self.wait_until(lambda: len(self.peer.outstanding()) > 0)
            # The second pong follows the getdata the node sent after answering the first.
            self.peer.sync_with_ping()
            self.peer.sync_with_ping()
            return len(self.peer.outstanding())

        self.log.info("A peer is asked for one block until its first block arrived")
        self.peer.sync_with_ping()
        self.peer.sync_with_ping()
        assert_equal(len(self.peer.outstanding()), 1)

        self.log.info("A peer that takes four seconds for a block is asked for one at a time")
        for _ in range(3):
            assert_equal(deliver_and_count(4), 1)
        assert node.getblockchaininfo()["initialblockdownload"]

        self.log.info("Once the peer delivers at once, it is asked for more, up to sixteen")
        counts = []
        while not counts or counts[-1] < MAX_BLOCKS_IN_TRANSIT_PER_PEER:
            counts.append(deliver_and_count(0))
            assert len(counts) < 20, f"the peer is still asked for {counts[-1]} blocks at a time: {counts}"
        self.log.info(f"Blocks asked for at a time after each delivery: {counts}")
        # The smoothed delivery time comes down from four seconds, so the count stays at one for a few deliveries,
        # and only grows from there.
        assert_equal(counts[0], 1)
        assert all(a <= b for a, b in zip(counts, counts[1:])), counts

        self.log.info("A peer that slows down again is asked for fewer")
        counts = []
        while not counts or counts[-1] > 1:
            counts.append(deliver_and_count(4))
            assert len(counts) < 10, f"the peer is still asked for {counts[-1]} blocks at a time: {counts}"
        self.log.info(f"Blocks asked for at a time after each delivery: {counts}")
        assert counts[0] < MAX_BLOCKS_IN_TRANSIT_PER_PEER, counts
        assert node.getblockchaininfo()["initialblockdownload"]


if __name__ == '__main__':
    IBDRequestRuleTest(__file__).main()
