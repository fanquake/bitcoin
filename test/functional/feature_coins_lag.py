#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The coins database lags the tip; a crash in the lag is recovered by replay.

With -test=write_out_every_block the node writes out the young tier's changes
after every connected block up to two blocks behind the tip, so the coins
database carries a head marker (new tip, written head) for the whole run
instead of only during a flush. A node with -dbcrashratio aborts inside those
writes at random. After every crash the node must come back on a consistent
tip by replaying the blocks between the heads. Blocks accepted after the run
started may not be in the durable block index yet, so the node may come back
a few blocks behind and the blocks are delivered again, as a peer would; the
UTXO set must then equal that of a node that never crashed, with reorgs mixed
in. A clean shutdown drains the lag: the database is then consistent with the
tip and carries no marker, which a restart without the test option must
accept.
"""

import time

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class CoinsLagTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = False
        self.rpc_timeout = 240
        self.crash_args = ["-test=write_out_every_block", "-dbcrashratio=6", "-dbcache=8", "-dbbatchsize=300", "-maxmempool=5"]
        self.extra_args = [
            # The crashing node: small cache, a write-out run after every block, simulated crashes inside them.
            self.crash_args,
            # The reference node.
            ["-dbcache=8"],
        ]

    def setup_network(self):
        self.add_nodes(self.num_nodes, extra_args=self.extra_args)
        self.start_nodes()
        # The nodes do not connect; blocks are submitted to the crashing node by hand.

    def restart_until_up(self):
        """Restart node 0 until it answers; each attempt may crash again inside a run (the replay's final
        write has the same crash hook)."""
        time_start = time.time()
        while time.time() - time_start < 120 * self.options.timeout_factor:
            try:
                self.start_node(0, extra_args=self.crash_args)
                self.nodes[0].getblockcount()
                return
            except Exception:
                self.wait_for_node_exit(0, timeout=10)
                self.crashed_on_restart += 1
        raise AssertionError("Unable to restart node 0 in the allotted time")

    def common_height(self):
        crashing, reference = self.nodes
        common = min(crashing.getblockcount(), reference.getblockcount())
        while common > 0 and reference.getblockhash(common) != crashing.getblockhash(common):
            common -= 1
        return common

    def sync_crashing_node(self):
        """Deliver the reference chain above the common ancestor to node 0; after a crash the node is restarted
        and the delivery resumes from wherever its replay left it."""
        reference = self.nodes[1]
        target = reference.getblockcount()
        while True:
            common = self.common_height()
            if common == target and self.nodes[0].getbestblockhash() == reference.getbestblockhash():
                return
            crashed = False
            for height in range(common + 1, target + 1):
                block = reference.getblock(reference.getblockhash(height), 0)
                try:
                    self.nodes[0].submitblock(block)
                except Exception as e:
                    self.log.debug(f"node 0 submitblock raised exception: {e}")
                    self.wait_for_node_exit(0, timeout=10)
                    self.crashes += 1
                    self.restart_until_up()
                    crashed = True
                    break
            if not crashed:
                assert_equal(self.nodes[0].getbestblockhash(), reference.getbestblockhash())
                return

    def utxo_hash(self):
        """Node 0's UTXO set hash. The RPC flushes the chainstate, which may crash the node inside a write;
        the node is then restarted, brought back to the tip and asked again."""
        for _ in range(20):
            try:
                return self.nodes[0].gettxoutsetinfo()["hash_serialized_3"]
            except Exception as e:
                self.log.debug(f"node 0 gettxoutsetinfo raised exception: {e}")
                self.wait_for_node_exit(0, timeout=10)
                self.crashes += 1
                self.restart_until_up()
                self.sync_crashing_node()
        raise AssertionError("node 0 kept crashing in gettxoutsetinfo")

    def run_test(self):
        crashing, reference = self.nodes
        self.crashes = 0
        self.crashed_on_restart = 0
        wallet = MiniWallet(reference)
        self.generate(reference, COINBASE_MATURITY + 10, sync_fun=self.no_op)
        self.sync_crashing_node()

        for round_ in range(6):
            # A burst of transactions mined into a few blocks, with a two-block reorg every third round.
            for _ in range(15):
                wallet.send_self_transfer(from_node=reference)
            if round_ % 3 == 2:
                tip = reference.getbestblockhash()
                parent = reference.getblockheader(tip)["previousblockhash"]
                reference.invalidateblock(parent)
                wallet.rescan_utxos()
                for _ in range(5):
                    wallet.send_self_transfer(from_node=reference)
            # A reorg round removed two blocks: the new branch must outgrow the old tip to be taken.
            self.generate(reference, 3 if round_ % 3 == 2 else 2, sync_fun=self.no_op)
            self.sync_crashing_node()
            assert_equal(self.utxo_hash(), reference.gettxoutsetinfo()["hash_serialized_3"])
        self.log.info(f"recovered from {self.crashes} crashes ({self.crashed_on_restart} on restart)")
        if self.crashes == 0:
            # Sized for speed: with this few writes the crash hook may not fire in a run; the UTXO set
            # comparison above still holds every round, and the recovery path is exercised most runs.
            self.log.warning("the crash hook did not fire in this run")

        # A clean shutdown drains the lag: a restart without the test option finds a consistent database.
        self.stop_node(0)
        self.start_node(0, extra_args=["-dbcache=8"])
        assert_equal(crashing.getbestblockhash(), reference.getbestblockhash())
        assert_equal(crashing.gettxoutsetinfo()["hash_serialized_3"], reference.gettxoutsetinfo()["hash_serialized_3"])


if __name__ == '__main__':
    CoinsLagTest(__file__).main()
