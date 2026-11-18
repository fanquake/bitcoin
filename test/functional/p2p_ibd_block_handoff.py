#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test the blocks of a peer that are processed on the connect pool during initial block download.

A block the node asked a peer for leaves the message handler thread. What the peer may rely on stays as it
was: a ping is answered only once the blocks sent before it are processed and connected, and a peer whose
block is found mutated or invalid is disconnected, with the chain left where it was.
"""

import copy
import time

from test_framework.blocktools import (
    TIME_GENESIS_BLOCK,
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlockHeader,
    MSG_BLOCK,
    MSG_TYPE_MASK,
    msg_block,
    msg_headers,
)
from test_framework.p2p import (
    P2PDataStore,
    P2PInterface,
    p2p_lock,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

# One getdata's worth: fewer than a peer may have in flight at once.
BATCH = 12


class msg_truncated_block:
    """A block message that ends after the header and a transaction count of one."""
    msgtype = b"block"

    def __init__(self, block):
        self.block = block

    def serialize(self):
        return CBlockHeader(self.block).serialize() + b"\x01"


class BlockRequests(P2PInterface):
    """A peer that records the blocks it is asked for and sends none of them on its own, but for the one
    it is given to answer with."""

    def __init__(self):
        super().__init__()
        self.requested = set()
        self.answered = None

    def on_getdata(self, message):
        for inv in message.inv:
            if (inv.type & MSG_TYPE_MASK) == MSG_BLOCK:
                self.requested.add(inv.hash)
                if self.answered is not None and inv.hash == self.answered.hash_int:
                    self.send_without_ping(msg_block(self.answered))

    def announce_and_wait_for_requests(self, blocks):
        self.send_without_ping(msg_headers([CBlockHeader(block) for block in blocks]))
        self.wait_until(lambda: all(block.hash_int in self.requested for block in blocks))

    def send_all(self, blocks):
        """The blocks in a single write."""
        self.send_raw_message(b"".join(self.build_message(msg_block(block)) for block in blocks))


class IBDBlockHandoffTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        # A block is handed over only with a connect pool thread to spare, whatever the default number is.
        self.extra_args = [["-prevoutfetchthreads=4"]]

    def build_blocks(self, prev_hash, height, count, *, first_coinbase_value=50, time_offset=0):
        """A chain of `count` blocks on `prev_hash`, dated just after genesis so that the node stays in
        initial block download."""
        blocks = []
        for i in range(count):
            coinbase = create_coinbase(height + i, nValue=first_coinbase_value if i == 0 else 50)
            block = create_block(prev_hash, coinbase, ntime=TIME_GENESIS_BLOCK + height + i + time_offset)
            block.solve()
            blocks.append(block)
            prev_hash = block.hash_int
        return blocks

    def handed_off(self, blocks, peer_id):
        return [f"block {block.hash_hex} of peer={peer_id} goes to the connect pool" for block in blocks]

    def deliver_first_block(self, peer, tip_hex, height):
        """A peer is asked for one block at a time until its first has arrived, and then for as many as it
        delivers in two seconds. The peer answers one request at once, so that the batch after it is asked
        for as a whole. Returns the new tip."""
        node = self.nodes[0]
        block = self.build_blocks(int(tip_hex, 16), height, 1)[0]
        with p2p_lock:
            peer.answered = block
        peer.send_without_ping(msg_headers([CBlockHeader(block)]))
        self.wait_until(lambda: node.getbestblockhash() == block.hash_hex)
        return block.hash_hex

    def sync_from_new_peer(self, blocks):
        """A fresh peer serves whatever of `blocks` the node still asks for."""
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PDataStore())
        with p2p_lock:
            for block in blocks:
                peer.block_store[block.hash_int] = block
            # The store answers a getheaders from its last block back.
            peer.last_block_hash = blocks[-1].hash_int
        peer.send_without_ping(msg_headers([CBlockHeader(block) for block in blocks]))
        self.wait_until(lambda: node.getbestblockhash() == blocks[-1].hash_hex)
        peer.peer_disconnect()
        peer.wait_for_disconnect()

    def run_test(self):
        node = self.nodes[0]
        # The request rule asks a peer for as many blocks as it delivers in two seconds. With the clock frozen a
        # delivery takes no time, so every peer may have a batch in flight however slow the machine.
        node.setmocktime(int(time.time()))
        self.log.info("Requested blocks go to the connect pool, and the ping after them is answered once they are connected")
        peer = node.add_p2p_connection(BlockRequests())
        peer_id = max(info["id"] for info in node.getpeerinfo())
        tip_hex = self.deliver_first_block(peer, node.getbestblockhash(), 1)
        height = 2
        blocks = self.build_blocks(int(tip_hex, 16), height, BATCH)
        peer.announce_and_wait_for_requests(blocks)
        with node.assert_debug_log(expected_msgs=self.handed_off(blocks, peer_id)):
            for block in blocks:
                peer.send_without_ping(msg_block(block))
            peer.sync_with_ping()
            # No waiting: the pong itself says the blocks are in.
            assert_equal(node.getbestblockhash(), blocks[-1].hash_hex)
        assert node.getblockchaininfo()["initialblockdownload"]

        self.log.info("A block that was not asked for stays on the message handler")
        height += BATCH
        unrequested = self.build_blocks(blocks[-1].hash_int, height, 1)
        with node.assert_debug_log(expected_msgs=[f"received block {unrequested[0].hash_hex}"], unexpected_msgs=["goes to the connect pool"]):
            peer.send_without_ping(msg_block(unrequested[0]))
            peer.sync_with_ping()
        assert_equal(node.getbestblockhash(), unrequested[0].hash_hex)
        tip_hex = unrequested[0].hash_hex
        height += 1

        self.log.info("A mutated block on the connect pool disconnects its peer and is not stored")
        blocks = self.build_blocks(int(tip_hex, 16), height, BATCH)
        mutated = copy.deepcopy(blocks[0])
        mutated.vtx[0].vout[0].nValue -= 1
        assert_equal(mutated.hash_int, blocks[0].hash_int)
        peer.announce_and_wait_for_requests(blocks)
        with node.assert_debug_log(expected_msgs=self.handed_off(blocks[:1], peer_id) + ["mutated block"]):
            # One write: the valid blocks behind it are on the wire before the verdict closes the connection.
            peer.send_all([mutated] + blocks[1:])
            peer.wait_for_disconnect()
        # Nothing of the batch can connect without its first block.
        assert_equal(node.getbestblockhash(), tip_hex)
        assert_raises_rpc_error(-1, "Block not available (not fully downloaded)", node.getblock, blocks[0].hash_hex)

        self.log.info("Another peer delivers the batch, the blocks dropped with the first peer included")
        self.sync_from_new_peer(blocks)
        tip_hex = blocks[-1].hash_hex
        height += BATCH

        self.log.info("A block on the connect pool that fails when it is connected disconnects its peer")
        peer = node.add_p2p_connection(BlockRequests())
        peer_id = max(info["id"] for info in node.getpeerinfo())
        tip_hex = self.deliver_first_block(peer, tip_hex, height)
        height += 1
        invalid = self.build_blocks(int(tip_hex, 16), height, BATCH, first_coinbase_value=51)
        peer.announce_and_wait_for_requests(invalid)
        with node.assert_debug_log(expected_msgs=self.handed_off(invalid[:1], peer_id) + ["bad-cb-amount"]):
            peer.send_all(invalid)
            peer.wait_for_disconnect()
        assert_equal(node.getbestblockhash(), tip_hex)

        self.log.info("The valid chain is still taken from another peer")
        blocks = self.build_blocks(int(tip_hex, 16), height, BATCH, time_offset=1)
        self.sync_from_new_peer(blocks)
        assert node.getblockchaininfo()["initialblockdownload"]
        tip_hex = blocks[-1].hash_hex
        height += BATCH

        self.log.info("A requested block that does not deserialize on the connect pool disconnects its peer")
        peer = node.add_p2p_connection(BlockRequests())
        peer_id = max(info["id"] for info in node.getpeerinfo())
        tip_hex = self.deliver_first_block(peer, tip_hex, height)
        height += 1
        blocks = self.build_blocks(int(tip_hex, 16), height, BATCH)
        peer.announce_and_wait_for_requests(blocks)
        with node.assert_debug_log(expected_msgs=self.handed_off(blocks[:1], peer_id) + [f"block {blocks[0].hash_hex} of peer={peer_id} does not deserialize, disconnecting"]):
            peer.send_without_ping(msg_truncated_block(blocks[0]))
            peer.wait_for_disconnect()
        assert_equal(node.getbestblockhash(), tip_hex)

        self.log.info("The block is asked for again and taken from another peer")
        self.sync_from_new_peer(blocks)


if __name__ == '__main__':
    IBDBlockHandoffTest(__file__).main()
