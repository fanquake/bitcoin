#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Pruning stays below the coins database's written head.

The coins database lags the tip and is replayed from its written head after a
crash, so the blocks above that head must stay on disk. A pruning node has a
large cache here and writes no coins while it syncs: the written head stays
where the node started. A request to prune above it must first write the coins
out up to the pruned height, and only then choose and record the files.

First the node is made to crash inside that write (-dbcrashratio=1 aborts at
the first batch boundary). Nothing may be recorded as pruned at that point:
the restart replays from the old written head and needs every block above it.
Then the request is repeated on a node that lags again, the node is killed
right after the files are gone, and the restart replays the blocks above the
pruned height.
"""
import http.client

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

PRUNE_ARGS = ["-prune=1", "-fastprune"]
# Small batches, so that a write-out run of a few hundred coins has a boundary to crash at.
CRASH_ARGS = PRUNE_ARGS + ["-dbcrashratio=1", "-dbbatchsize=300"]


class CoinsLagPruneTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [CRASH_ARGS, []]

    def utxo_hash(self, node):
        return node.gettxoutsetinfo()["hash_serialized_3"]

    def run_test(self):
        pruning, reference = self.nodes
        start_height = pruning.getblockcount()

        self.log.info("Sync 800 blocks: nothing is written to the coins database meanwhile")
        self.generate(reference, 800)
        tip_height = start_height + 800
        prune_height = tip_height - 300
        assert_equal(pruning.getblockcount(), tip_height)
        assert_equal(pruning.getblockchaininfo()["pruned"], True)

        self.log.info("A prune request above the written head writes the coins out first; the node crashes in that write")
        with pruning.assert_debug_log([f"Starting a coins write-out run: changes at or below height {prune_height}", "Simulating a crash"], unexpected_msgs=["deleted blk/rev"]):
            crashed = False
            try:
                pruning.pruneblockchain(prune_height)
            except (http.client.CannotSendRequest, http.client.RemoteDisconnected, OSError) as e:
                self.log.debug(f"pruneblockchain raised: {e}")
                crashed = True
            assert crashed, "the node pruned above its written head without writing the coins out first"
            self.wait_for_node_exit(0, timeout=10 * self.options.timeout_factor)

        self.log.info("The restart replays from the old written head: no block above it was recorded as pruned")
        with pruning.assert_debug_log([f"Rolling forward to {reference.getbestblockhash()}"]):
            self.start_node(0, extra_args=PRUNE_ARGS)
        assert_equal(pruning.getbestblockhash(), reference.getbestblockhash())
        assert_equal(pruning.getblockchaininfo()["pruneheight"], 0)
        assert_equal(self.utxo_hash(pruning), self.utxo_hash(reference))

        self.log.info("Lag again, prune, and kill the node once the files are gone")
        self.connect_nodes(0, 1)
        self.generate(reference, 800)
        tip_height += 800
        prune_height = tip_height - 300
        with pruning.assert_debug_log([f"Starting a coins write-out run: changes at or below height {prune_height}"]):
            pruning.pruneblockchain(prune_height)
        # The first block file, at the least, lies below the pruned height.
        assert_greater_than(pruning.getblockchaininfo()["pruneheight"], start_height)
        pruning.kill_process()

        self.log.info("The restart replays the blocks above the pruned height")
        with pruning.assert_debug_log(["Rolling forward to"]):
            self.start_node(0, extra_args=PRUNE_ARGS)
        assert_equal(pruning.getbestblockhash(), reference.getbestblockhash())
        assert_equal(self.utxo_hash(pruning), self.utxo_hash(reference))


if __name__ == '__main__':
    CoinsLagPruneTest(__file__).main()
