#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Undo data is written after a block is connected; check the node survives losing it.

With -test=crash_before_undo_write the node aborts when its first undo write
is about to run, that is after the block was published as the tip but before
its undo data exists. Nothing of that block may survive the restart as
"connected": the node must come back on the previous tip, with a consistent
chainstate, and must be able to connect the same block again afterwards.
Also checks that undo data is readable right after a block is connected, so
the readers that settle the executor first (getblock, getblockstats, the
indexes) see it.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class AsyncUndoTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [[], ["-txindex=1", "-coinstatsindex=1"]]

    def run_test(self):
        node, other = self.nodes
        wallet = MiniWallet(node)
        self.generate(wallet, 20)
        self.generate(node, 100)

        self.log.info("Undo data of a just-connected block is available to readers that settle the executor")
        for _ in range(5):
            wallet.send_self_transfer(from_node=node)
        block_hash = self.generate(node, 1)[0]
        verbose = node.getblock(block_hash, 2)
        assert all("fee" in tx for tx in verbose["tx"][1:])
        stats = node.getblockstats(block_hash)
        assert_equal(stats["txs"], 6)
        other.syncwithvalidationinterfacequeue()
        assert_equal(other.gettxoutsetinfo("muhash")["muhash"], node.gettxoutsetinfo("muhash")["muhash"])
        assert_equal(other.getindexinfo("coinstatsindex")["coinstatsindex"]["synced"], True)

        self.log.info("Crash before the undo write of a new block")
        tip_before = node.getbestblockhash()
        height_before = node.getblockcount()
        self.disconnect_nodes(0, 1)
        self.restart_node(0, extra_args=["-test=crash_before_undo_write"])
        for _ in range(3):
            wallet.send_self_transfer(from_node=node)
        try:
            self.generate(node, 1, sync_fun=self.no_op)
        except Exception:  # noqa: BLE001 - the node dies during this call
            pass
        node.wait_until_stopped(timeout=60, expected_ret_code=-6)

        self.log.info("The node restarts on the previous tip with a consistent chainstate")
        self.start_node(0)
        assert_equal(node.getbestblockhash(), tip_before)
        assert_equal(node.getblockcount(), height_before)
        assert node.verifychain(checklevel=4, nblocks=0)

        self.log.info("And connects new blocks, with their undo data, afterwards")
        self.connect_nodes(0, 1)
        self.sync_blocks()
        wallet.rescan_utxos()
        for _ in range(3):
            wallet.send_self_transfer(from_node=node)
        new_hash = self.generate(node, 1)[0]
        assert all("fee" in tx for tx in node.getblock(new_hash, 2)["tx"][1:])
        assert_equal(other.getbestblockhash(), new_hash)
        assert node.verifychain(checklevel=4, nblocks=0)


if __name__ == "__main__":
    AsyncUndoTest(__file__).main()
