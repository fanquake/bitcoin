#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Validate several blocks at once during initial block download (the connect window).

A node syncing a chain of transaction-heavy blocks validates up to -connectwindow
of them in parallel, each against its own view of the UTXO set, with a transaction
whose input a lower block has not produced yet deferred to its block's
confirmation. The confirmed chain and UTXO set must be those of validating the
blocks one at a time, an invalid block inside the window must fail with the same
reason and leave the node on the best valid chain, and the node must still take
the rest of the chain afterwards.
"""

import random
import re
import time

from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlock,
    CBlockHeader,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    from_hex,
    msg_headers,
)
from test_framework.p2p import P2PDataStore
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet

# Sized for a few seconds: enough blocks for the window to fill and overlap, not a benchmark.
NUM_TX_BLOCKS = 12
TXS_PER_BLOCK = 25
FORK_LENGTH = 6
INVALID_AT = 3  # the invalid block's position in the fork, 1-based


class ConnectWindowTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        # The framework gives every node one connect thread; the window needs several to overlap.
        self.extra_args = [
            [],
            ["-connectwindow=4", "-prevoutfetchthreads=4"],
            ["-connectwindow=1", "-prevoutfetchthreads=4"],
            ["-connectwindow=4", "-prevoutfetchthreads=4"],
        ]

    def setup_network(self):
        # Nodes are connected once the source chain exists, so that they sync it in initial block download.
        self.setup_nodes()

    def build_source_chain(self):
        node = self.nodes[0]
        # Blocks dated two days back keep the syncing nodes in initial block download for the whole chain.
        self.mocktime = int(time.time()) - 2 * 24 * 3600
        node.setmocktime(self.mocktime)
        wallet = MiniWallet(node)
        self.generate(wallet, 101, sync_fun=self.no_op)
        self.log.info(f"Mining {NUM_TX_BLOCKS} blocks of {TXS_PER_BLOCK} chained transactions each")
        for _ in range(NUM_TX_BLOCKS):
            # Each chain spends the previous block's output first, so every block depends on the one below.
            wallet.send_self_transfer_chain(from_node=node, chain_length=TXS_PER_BLOCK)
            self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.getmempoolinfo()["size"], 0)

    def check_same_state(self, nodes):
        best = nodes[0].getbestblockhash()
        utxo = nodes[0].gettxoutsetinfo()["hash_serialized_3"]
        for node in nodes[1:]:
            assert_equal(node.getbestblockhash(), best)
            assert_equal(node.gettxoutsetinfo()["hash_serialized_3"], utxo)

    def stop_and_check_window_used(self, i, used):
        """Stop the node and check its shutdown summary: the window's counters are logged only if it was used."""
        expected = ["Connect window:"] if used else []
        unexpected = [] if used else ["Connect window:"]
        with self.nodes[i].assert_debug_log(expected_msgs=expected, unexpected_msgs=unexpected):
            self.stop_node(i)
        if used:
            with open(self.nodes[i].debug_log_path, encoding="utf-8") as log:
                summary = [line for line in log if "Connect window:" in line][-1]
            blocks, deferred = (int(n) for n in re.search(r"(\d+) blocks validated.*?(\d+) transactions deferred", summary).groups())
            assert blocks > 0
            # How many transactions waited for a lower block depends on the workers' timing; it is reported,
            # not required, since the state checks above hold either way.
            self.log.info(f"node {i}: {blocks} blocks in the window, {deferred} transactions deferred")

    def test_sync(self):
        self.log.info("Sync the chain with a window of four and of one, and compare chains and UTXO sets")
        for i in (1, 2):
            self.connect_nodes(i, 0)
        self.sync_blocks(self.nodes[:3])
        self.check_same_state(self.nodes[:3])
        self.stop_and_check_window_used(1, used=True)
        self.stop_and_check_window_used(2, used=False)

    def test_invalid_block_in_window(self):
        node0, node3 = self.nodes[0], self.nodes[3]
        height = node0.getblockcount()
        self.log.info("Serve a fresh node the chain and a longer fork with an invalid block inside it")

        main_blocks = [from_hex(CBlock(), node0.getblock(node0.getblockhash(h), 0)) for h in range(1, height + 1)]
        fork_base_height = height - FORK_LENGTH // 2
        fork_base = main_blocks[fork_base_height - 1]
        fork_blocks = []
        prev_hash, prev_time = fork_base.hash_int, fork_base.nTime
        for n in range(1, FORK_LENGTH + 1):
            h = fork_base_height + n
            txlist = []
            if n == INVALID_AT:
                # Spends an output that does not exist: with the blocks below still being validated this is
                # deferred to the block's confirmation, where it fails as it would have failed at once.
                bogus = CTransaction()
                bogus.vin.append(CTxIn(COutPoint(random.getrandbits(256), 0), b""))
                bogus.vout.append(CTxOut(1, CScript([OP_TRUE])))
                txlist.append(bogus)
            block = create_block(prev_hash, create_coinbase(h), ntime=prev_time + 1, txlist=txlist)
            block.solve()
            fork_blocks.append(block)
            prev_hash, prev_time = block.hash_int, block.nTime

        peer = node3.add_p2p_connection(P2PDataStore())
        for block in main_blocks + fork_blocks:
            peer.block_store[block.hash_int] = block
        peer.last_block_hash = fork_blocks[-1].hash_int
        # Both branches' headers: the fork has more work and is validated first, up to its invalid block.
        with node3.assert_debug_log(["bad-txns-inputs-missingorspent"]):
            peer.send_without_ping(msg_headers(headers=[CBlockHeader(b) for b in main_blocks]))
            peer.send_without_ping(msg_headers(headers=[CBlockHeader(b) for b in fork_blocks]))
            self.wait_until(lambda: any(t["status"] == "invalid" for t in node3.getchaintips()), timeout=120)
        # The fork's blocks below the invalid one were valid and stay connected: the node ends on the last of
        # them, and nothing of the invalid block or those above it is left in its state.
        tips = {t["hash"]: t["status"] for t in node3.getchaintips()}
        assert_equal(tips[fork_blocks[-1].hash_hex], "invalid")
        assert_equal(node3.getbestblockhash(), fork_blocks[INVALID_AT - 2].hash_hex)
        assert_equal(node3.getblockcount(), fork_base_height + INVALID_AT - 1)
        # A peer whose best block is invalid is not asked for more (as on master), so the source node
        # supplies the main chain.
        node3.disconnect_p2ps()

        self.log.info("The node reorgs to the main chain and keeps taking blocks afterwards")
        self.connect_nodes(3, 0)
        self.generate(node0, 3, sync_fun=self.no_op)
        self.sync_blocks([node0, node3])
        self.check_same_state([node0, node3])
        self.stop_and_check_window_used(3, used=True)

    def run_test(self):
        self.build_source_chain()
        self.test_sync()
        self.test_invalid_block_in_window()


if __name__ == '__main__':
    ConnectWindowTest(__file__).main()
