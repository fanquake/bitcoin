#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the connect-side block cache.

A block that arrives before its parent is stored but cannot be connected
yet. When the parent arrives, the connect loop takes the stored block from
the in-memory cache (the object that was received and checked) instead of
reading it back from disk; with the cache disabled it reads it from disk.
"""

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader, msg_block, msg_headers
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

CACHED = "Using block from the cache"


class BlockCacheTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [["-debug=bench", "-blockcache=16"], ["-debug=bench", "-blockcache=0"]]

    def make_chain(self, node, length):
        tip = int(node.getbestblockhash(), 16)
        height = node.getblockcount()
        time = node.getblock(node.getbestblockhash())["time"] + 1
        blocks = []
        for i in range(length):
            block = create_block(tip, create_coinbase(height + 1 + i), ntime=time + i)
            block.solve()
            blocks.append(block)
            tip = block.hash_int
        return blocks

    def run_test(self):
        for node, cached in ((self.nodes[0], True), (self.nodes[1], False)):
            self.log.info(f"Deliver blocks out of order to a node with the block cache {'on' if cached else 'off'}")
            peer = node.add_p2p_connection(P2PInterface())
            start_height = node.getblockcount()
            blocks = self.make_chain(node, 3)
            # Announce the three headers, then deliver the third and the second block: they are stored, but their
            # parent has no data yet, so neither connects.
            peer.send_and_ping(msg_headers(headers=[CBlockHeader(block) for block in blocks]))
            peer.send_and_ping(msg_block(blocks[2]))
            peer.send_and_ping(msg_block(blocks[1]))
            assert_equal(node.getblockcount(), start_height)
            # The first block connects all three. The first is the object just received; the other two come from
            # the cache, or are read from disk when the cache is off. (The "Load block from disk" timing line is
            # logged for every block, whichever way it was obtained, so only the cache line tells them apart.)
            expected, unexpected = ([CACHED], []) if cached else ([], [CACHED])
            with node.assert_debug_log(expected_msgs=expected, unexpected_msgs=unexpected):
                peer.send_and_ping(msg_block(blocks[0]))
            assert_equal(node.getblockcount(), start_height + 3)
            assert_equal(node.getbestblockhash(), blocks[2].hash_hex)
            node.disconnect_p2ps()


if __name__ == "__main__":
    BlockCacheTest(__file__).main()
