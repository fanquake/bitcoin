#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a block a peer sends at the tip is connected by the connector thread.

Outside initial block download the message handler accepts and stores a new block and the connector thread
connects it, from the block in hand even without the block cache, while the handler goes on with the other
peers. A ping is still answered only once the block the peer sent before it is connected, and a transaction
that follows a block is judged once that block is connected: a child of one of the block's transactions sent
right behind it enters the mempool, not the orphanage.
"""

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import msg_block, msg_tx
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class TipConnectorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        # Without the block cache, a block the connector did not get in hand would be read back from disk.
        self.extra_args = [["-logthreadnames=1", "-debug=bench", "-debug=txpackages", "-blockcache=0"]]

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 1)
        assert_equal(node.getblockchaininfo()["initialblockdownload"], False)
        peer = node.add_p2p_connection(P2PInterface())

        self.log.info("A block at the tip is connected by the connector thread, from the block in hand")
        for _ in range(3):
            tip = node.getbestblockhash()
            height = node.getblockcount()
            block = create_block(int(tip, 16), create_coinbase(height + 1), ntime=node.getblock(tip)["time"] + 1)
            block.solve()
            with open(node.debug_log_path, encoding="utf-8") as log:
                log.seek(0, 2)
                start = log.tell()
            # The pong comes once the block is connected: the tip is the block right after it.
            peer.send_and_ping(msg_block(block))
            assert_equal(node.getbestblockhash(), block.hash_hex)
            with open(node.debug_log_path, encoding="utf-8") as log:
                log.seek(start)
                lines = log.read().splitlines()
            update = [line for line in lines if f"UpdateTip: new best={block.hash_hex}" in line]
            assert_equal(len(update), 1)
            assert " [connect] " in update[0], update[0]
            # Taken as the block in hand, not read back (the cache is off).
            assert any(" [connect] " in line and "Using cached block" in line for line in lines)

        self.log.info("A transaction right behind a block that creates its input is judged after the block")
        wallet = MiniWallet(node)
        self.generate(wallet, 105)
        # Several rounds: without the wait, whether the child comes before the connection is a race.
        for _ in range(5):
            parent = wallet.create_self_transfer()
            child = wallet.create_self_transfer(utxo_to_spend=parent["new_utxo"])
            tip = node.getbestblockhash()
            block = create_block(int(tip, 16), create_coinbase(node.getblockcount() + 1), ntime=node.getblock(tip)["time"] + 1, txlist=[parent["tx"]])
            add_witness_commitment(block)
            block.solve()
            # The node never saw the parent: the child's input exists only once the block is connected.
            assert parent["txid"] not in node.getrawmempool()
            # Judged before the block were connected, the child would be stored as an orphan first (and taken
            # from there once the block is connected).
            with node.assert_debug_log(expected_msgs=[], unexpected_msgs=["stored orphan tx"]):
                peer.send_without_ping(msg_block(block))
                peer.send_and_ping(msg_tx(child["tx"]))
            assert_equal(node.getbestblockhash(), block.hash_hex)
            assert child["txid"] in node.getrawmempool()


if __name__ == '__main__':
    TipConnectorTest(__file__).main()
