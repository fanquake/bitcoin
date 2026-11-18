#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a crash after a reorganisation that restores a coin the coins database never held.

With -test=write_out_every_block the coins database is written two blocks behind the tip after every block.
A coin created in block c and spent in block c+2 is created and spent inside the lag, so its create and its
spend cancel and the database never holds it, although it is alive at block c, the height the database then
settles at. Disconnecting block c+2 restores the coin. The node is then killed after a further block, and
the coin must still be there after the restart (block c+2 is invalidated again, since the invalidation
itself need not have reached the disk).

This is an end-to-end check of the restart, not of the restored-coin write: at the option's constant lag no
run has a bound between the settled height and the fork, which is where a run would settle the database
without the coin. The unit case disconnect_restores_a_coin_the_database_never_held drives such a run.
"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class CoinsLagReorgTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-test=write_out_every_block", "-debug=coindb"]]

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)
        output = wallet.get_address()

        self.log.info("Create a coin in block c and spend it in block c+2")
        create = wallet.create_self_transfer()
        block_c = self.generateblock(node, output, [create["hex"]])["hash"]
        block_c1 = self.generateblock(node, output, [])["hash"]
        spend = wallet.create_self_transfer(utxo_to_spend=create["new_utxo"])
        block_c2 = self.generateblock(node, output, [spend["hex"]])["hash"]
        node.syncwithvalidationinterfacequeue()
        coin = (create["txid"], create["new_utxo"]["vout"])
        assert node.gettxout(*coin, include_mempool=False) is None
        height_c = node.getblockheader(block_c)["height"]

        self.log.info("Disconnect block c+2: the coin is back")
        node.invalidateblock(block_c2)
        assert_equal(node.gettxout(*coin, include_mempool=False)["bestblock"], node.getbestblockhash())

        self.log.info("A block on the fork, without the spend, and its write-out")
        fork_tip = self.generateblock(node, output, [])["hash"]
        node.syncwithvalidationinterfacequeue()
        assert_equal(node.getblockcount(), height_c + 2)

        self.log.info("Kill the node and restart it: the coin is still there")
        node.kill_process()
        self.start_node(0)
        # The invalidation may not have reached the disk before the kill, and block c+2 has as much work as the
        # fork's tip: the node may be back on c+2, which is valid only if the coin it spends is there.
        node.invalidateblock(block_c2)
        # The fork's block is lost too when its index entry did not reach the disk; the coin is alive at either.
        assert node.getbestblockhash() in (fork_tip, block_c1)
        assert node.gettxout(*coin, include_mempool=False) is not None, "the restored coin was lost in the crash"


if __name__ == '__main__':
    CoinsLagReorgTest(__file__).main()
