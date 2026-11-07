#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Read the chainstate and feed the mempool while blocks are being connected.

Block validation runs with cs_main released; readers that take the lock see
either the previous or the new tip and a coins view consistent with it. This
test keeps several RPC readers and a transaction submitter busy on a node
while it syncs a chain of transaction-heavy blocks, then checks that the node
ends up with the same chain and UTXO set as its source. It is most useful
under a thread sanitizer or lock-order checking build.
"""

from threading import Event, Thread

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet

# Sized for a few seconds per node: enough blocks of signature work for the readers to overlap with.
NUM_TX_BLOCKS = 6
TXS_PER_BLOCK = 6
INPUTS_PER_TX = 8
NUM_LATE_TXS = 8

# RPC errors a reader may legitimately see while the node is behind the source.
EXPECTED_RPC_ERRORS = {
    -5,   # Block not found, no such mempool or blockchain transaction
    -25,  # Missing inputs
    -26,  # Rejected by mempool policy (e.g. already in mempool while a resend is in flight)
    -27,  # Already in the chain
}


class ConnectConcurrencyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.extra_args = [
            [],
            ["-checkmempool=1"],
            ["-checkmempool=1", "-prevoutfetchthreads=0"],
        ]

    def setup_network(self):
        # Nodes are connected one at a time once the source chain exists.
        self.setup_nodes()

    def reader(self, node, stop, hashes, outpoints, late_txs, errors, counts):
        rpc = node.create_new_rpc_connection()
        i = 0
        while not stop.is_set():
            i += 1
            try:
                rpc.getbestblockhash()
                rpc.getblockchaininfo()
                rpc.getrawmempool()
                outpoint = outpoints[i % len(outpoints)]
                rpc.gettxout(outpoint[0], outpoint[1], True)
                rpc.getblock(hashes[i % len(hashes)], 1)
                if late_txs:
                    tx = late_txs[i % len(late_txs)]
                    rpc.sendrawtransaction(tx)
                    counts["accepted"] += 1
                counts["calls"] += 1
            except JSONRPCException as e:
                # getblock on a block the node has the header of but not the data yet.
                header_only = e.error["code"] == -1 and "Block not available" in e.error["message"]
                if e.error["code"] not in EXPECTED_RPC_ERRORS and not header_only:
                    errors.append(e.error)
                    return
                counts["expected_errors"] += 1
            except Exception as e:  # noqa: BLE001 - any other failure fails the test
                errors.append(repr(e))
                return

    def sync_with_readers(self, node, hashes, outpoints, late_txs):
        stop = Event()
        errors = []
        counts = {"calls": 0, "expected_errors": 0, "accepted": 0}
        threads = [
            Thread(target=self.reader, args=(node, stop, hashes, outpoints, late_txs if n == 0 else [], errors, counts))
            for n in range(3)
        ]
        for t in threads:
            t.start()
        self.connect_nodes(node.index, 0)
        try:
            self.sync_blocks([self.nodes[0], node], timeout=240)
        finally:
            stop.set()
            for t in threads:
                t.join()
        self.log.info(f"readers: {counts}")
        assert_equal(errors, [])
        assert counts["calls"] > 0

        # Everything the readers were refused is accepted once the node has caught up.
        for tx in late_txs:
            try:
                node.sendrawtransaction(tx)
            except JSONRPCException as e:
                assert_equal(e.error["code"], -27)  # already there
        assert_equal(node.getbestblockhash(), self.nodes[0].getbestblockhash())
        assert_equal(node.gettxoutsetinfo("muhash")["muhash"], self.nodes[0].gettxoutsetinfo("muhash")["muhash"])
        mempool = node.getrawmempool()
        for txid in self.late_txids:
            assert txid in mempool

    def run_test(self):
        node0 = self.nodes[0]
        # The default taproot script-path spends cost an EC operation per input to verify, so
        # connecting the blocks takes script verification work.
        wallet = MiniWallet(node0)
        self.log.info("Build a source chain whose blocks take some work to connect")
        self.generate(wallet, 10, sync_fun=self.no_op)
        self.generate(node0, 100, sync_fun=self.no_op)
        # Split the mature coinbases into many outputs so every later transaction spends confirmed coins.
        for _ in range(10):
            wallet.send_self_transfer_multi(from_node=node0, num_outputs=NUM_TX_BLOCKS * TXS_PER_BLOCK * INPUTS_PER_TX // 10 + NUM_LATE_TXS, confirmed_only=True)
        self.generate(node0, 1, sync_fun=self.no_op)
        wallet.rescan_utxos()
        # Many signatures per transaction: script work for the syncing node, few RPC calls for the test.
        for _ in range(NUM_TX_BLOCKS):
            for _ in range(TXS_PER_BLOCK):
                utxos = [wallet.get_utxo(confirmed_only=True) for _ in range(INPUTS_PER_TX)]
                wallet.send_self_transfer_multi(from_node=node0, utxos_to_spend=utxos, num_outputs=INPUTS_PER_TX)
            self.generate(node0, 1, sync_fun=self.no_op)
        hashes = [node0.getblockhash(h) for h in range(0, node0.getblockcount() + 1, 7)]
        outpoints = [(u["txid"], u["vout"]) for u in wallet.get_utxos(mark_as_spent=False)[:50]]

        self.log.info("Prepare transactions that spend outputs the syncing nodes do not know yet")
        late = [wallet.create_self_transfer(confirmed_only=True) for _ in range(NUM_LATE_TXS)]
        late_txs = [t["hex"] for t in late]
        self.late_txids = [t["txid"] for t in late]

        for node in self.nodes[1:]:
            self.log.info(f"Sync node {node.index} with {node.extra_args} while readers and a submitter hammer it")
            self.sync_with_readers(node, hashes, outpoints, late_txs)
            self.disconnect_nodes(node.index, 0)


if __name__ == "__main__":
    ConnectConcurrencyTest(__file__).main()
