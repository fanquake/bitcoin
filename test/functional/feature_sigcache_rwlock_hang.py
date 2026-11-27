#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.
"""Demonstrate the cs_sigcache deadlock on mingw-w64 (winpthreads) builds.

getblocktemplate proposals run TestBlockValidity, whose parallel script check
threads call SignatureCache::Get and ::Set concurrently. With a tiny cache
every signature misses, so every check does both.

The signature cache is never used before the first proposal (funding only
spends bare OP_TRUE outputs), so its first use is concurrent and can hit the
winpthreads PTHREAD_RWLOCK_INITIALIZER first-use race, which leaves the lock
broken. Each restart of bitcoind is a fresh attempt.

On an affected Windows build a proposal eventually never returns (RPC timeout)
and bitcoind is left deadlocked holding cs_main. Elsewhere the test passes.
"""

import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.key import ECKey
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import sign_input_legacy
from test_framework.script_util import key_to_p2pk_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet, MiniWalletMode

NUM_SIGS = 3000          # signatures per proposal
INPUTS_PER_TX = 25
STARTS = 50              # bitcoind starts (each one races the first use again)
SECONDS_PER_START = 20


class SigCacheRwlockHangTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-par=16", "-maxsigcachesize=0"]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Create P2PK outputs without any signature checks")
        op_true = MiniWallet(node, mode=MiniWalletMode.RAW_OP_TRUE)
        self.generate(op_true, 101)
        key = ECKey()
        key.generate()
        p2pk = key_to_p2pk_script(key.get_pubkey().get_bytes())
        value = 1_000_000
        utxo = op_true.get_utxo()
        fund = CTransaction()
        fund.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        fund.vout = [CTxOut(value, p2pk) for _ in range(NUM_SIGS)]
        op_true.sign_tx(fund)
        self.generateblock(node, output=op_true.get_descriptor(), transactions=[fund.serialize().hex()])

        self.log.info(f"Build a block proposal spending them ({NUM_SIGS} signatures)")
        txs = []
        for start in range(0, NUM_SIGS, INPUTS_PER_TX):
            tx = CTransaction()
            tx.vin = [CTxIn(COutPoint(fund.txid_int, n)) for n in range(start, start + INPUTS_PER_TX)]
            tx.vout = [CTxOut(INPUTS_PER_TX * value - 10_000, p2pk)]
            for i in range(INPUTS_PER_TX):
                sign_input_legacy(tx, i, p2pk, key)
            txs.append(tx)
        tip = node.getbestblockhash()
        block = create_block(int(tip, 16), create_coinbase(node.getblockcount() + 1), txlist=txs)
        proposal = {"mode": "proposal", "data": block.serialize().hex()}

        self.log.info(f"Submit proposals for {SECONDS_PER_START}s per bitcoind start, {STARTS} starts")
        for n in range(1, STARTS + 1):
            if n > 1:
                self.restart_node(0)
            deadline = time.time() + SECONDS_PER_START
            while time.time() < deadline:
                # On an affected build this eventually times out: bitcoind is deadlocked.
                assert_equal(self.nodes[0].getblocktemplate(proposal), None)
            self.log.info(f"start #{n}: no hang")


if __name__ == '__main__':
    SigCacheRwlockHangTest(__file__).main()
