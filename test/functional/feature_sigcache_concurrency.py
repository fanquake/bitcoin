#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.
"""Stress concurrent signature cache lookups and inserts.

Block proposals (getblocktemplate mode=proposal) run TestBlockValidity, whose
parallel script check workers both look up (SignatureCache::Get) and insert
(SignatureCache::Set) signatures at the same time. With -maxsigcachesize=0 the
cache only holds a couple of entries, so every proposal misses and inserts
again.

The signature cache is never used before the first proposal (funding only
spends bare OP_TRUE outputs), so its first use is concurrent. On mingw-w64
builds with an affected winpthreads (PTHREAD_RWLOCK_INITIALIZER first-use
race, mingw-w64 bugs #883/#1044) that either aborts in
std::__shared_mutex_pthread (builds with libstdc++ assertions) or leaves the
rwlock broken, which can eventually deadlock the node.
"""

import time

from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.key import ECKey
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.script import sign_input_legacy
from test_framework.script_util import key_to_p2pk_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import (
    MiniWallet,
    MiniWalletMode,
)


class SigCacheConcurrencyTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--duration", type=int, default=14400, help="Seconds to keep submitting proposals (default: %(default)s)")
        parser.add_argument("--utxos", type=int, default=3000, help="Signatures per proposal (default: %(default)s)")
        parser.add_argument("--par", type=int, default=16, help="-par for the node; higher values are capped at 16 (default: %(default)s)")
        parser.add_argument("--restart-every", type=int, default=20, help="Restart bitcoind every N seconds, so each start races the first use of the cache again; 0 = never (default: %(default)s)")
        parser.add_argument("--keep-hung", action="store_true", help="Leave a hung bitcoind running for inspection (e.g. winedbg) instead of killing it")

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.rpc_timeout = 60

    def setup_network(self):
        self.extra_args = [[f"-par={self.options.par}", "-maxsigcachesize=0"]]
        self.setup_nodes()

    def log_instrumentation(self, node):
        """Print what the cs_sigcache instrumentation (if built in) logged."""
        with open(node.debug_log_path, encoding="utf-8", errors="replace") as f:
            lines = [line.rstrip() for line in f if "sigcache rwlock" in line or "Script verification uses" in line]
        if any("sigcache rwlock" in line for line in lines):
            self.log.info("cs_sigcache instrumentation:\n" + "\n".join(lines))
        else:
            self.log.info("No cs_sigcache instrumentation output (no lock errors seen, or the instrumentation isn't built in):\n" + "\n".join(lines))

    def fail_with_node_state(self, node, what, exc):
        self.log_instrumentation(node)
        rc = node.process.poll()
        if rc is None:
            self.log.error(f"{what} did not return ({exc!r}) and bitcoind is still running: treating as a deadlock")
            if self.options.keep_hung:
                self.log.error(f"Leaving bitcoind (pid {node.process.pid}) running for inspection, datadir {node.datadir_path}")
            else:
                node.process.kill()
                node.process.wait()
            reason = "bitcoind stopped responding (deadlock?)"
        else:
            reason = f"bitcoind exited with code {rc}"
        node.stderr.seek(0)
        stderr = node.stderr.read().decode("utf-8", errors="replace").strip()
        if stderr:
            self.log.error(f"bitcoind stderr:\n{stderr}")
        # Mark the node as stopped so shutdown doesn't try to stop it again.
        node.running = False
        node.process = None
        node.rpc_connected = False
        node._rpc = None
        raise AssertionError(f"{reason} during {what}")

    def run_test(self):
        node = self.nodes[0]
        n_utxos = self.options.utxos
        inputs_per_tx = 25

        self.log.info("Fund P2PK outputs using only bare OP_TRUE spends (no signature checks)")
        op_true = MiniWallet(node, mode=MiniWalletMode.RAW_OP_TRUE)
        self.generate(op_true, 101)
        key = ECKey()
        key.generate()
        p2pk = key_to_p2pk_script(key.get_pubkey().get_bytes())
        value = 1_000_000  # 0.01 BTC
        utxo = op_true.get_utxo()
        assert int(utxo["value"] * COIN) > n_utxos * value
        fund = CTransaction()
        fund.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        fund.vout = [CTxOut(value, p2pk) for _ in range(n_utxos)]
        op_true.sign_tx(fund)
        self.generateblock(node, output=op_true.get_descriptor(), transactions=[fund.serialize().hex()])

        self.log.info(f"Sign {n_utxos} P2PK inputs for the proposal (in Python, may take a while)")
        txs = []
        for start in range(0, n_utxos, inputs_per_tx):
            vouts = range(start, min(start + inputs_per_tx, n_utxos))
            tx = CTransaction()
            tx.vin = [CTxIn(COutPoint(fund.txid_int, n)) for n in vouts]
            tx.vout = [CTxOut(len(vouts) * value - 10_000, p2pk)]
            for i in range(len(tx.vin)):
                sign_input_legacy(tx, i, p2pk, key)
            txs.append(tx)

        tip = node.getbestblockhash()
        height = node.getblockcount() + 1
        block = create_block(int(tip, 16), create_coinbase(height), txlist=txs)
        proposal = {"mode": "proposal", "data": block.serialize().hex()}

        self.log.info(f"Submit proposals for {self.options.duration}s with -par={self.options.par}, -maxsigcachesize=0")
        deadline = time.time() + self.options.duration
        count = 0
        start = 1
        start_time = time.time()
        while time.time() < deadline:
            if self.options.restart_every and time.time() - start_time > self.options.restart_every:
                self.restart_node(0)
                node = self.nodes[0]
                start += 1
                start_time = time.time()
                if start % 10 == 0:
                    self.log.info(f"bitcoind start #{start}, {count} proposals so far")
            t0 = time.time()
            try:
                result = node.getblocktemplate(proposal)
            except Exception as e:
                self.fail_with_node_state(node, f"proposal #{count + 1} (bitcoind start #{start})", e)
            assert_equal(result, None)
            count += 1
            if count == 1 or count % 250 == 0:
                self.log.info(f"proposal #{count} ok ({time.time() - t0:.2f}s)")

        self.log.info(f"No abort or hang after {count} proposals ({count * n_utxos} signature checks) over {start} bitcoind starts")
        self.log_instrumentation(node)
        # Make sure cs_main is still free.
        assert_equal(node.getbestblockhash(), tip)


if __name__ == '__main__':
    SigCacheConcurrencyTest(__file__).main()
