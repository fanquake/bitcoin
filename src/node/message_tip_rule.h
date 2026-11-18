// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_MESSAGE_TIP_RULE_H
#define BITCOIN_NODE_MESSAGE_TIP_RULE_H

#include <protocol.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace node {
/**
 * What a peer's message needs of the blocks the message handler accepted
 * before it (precondition O1 of validation.h): the handler hands a block to
 * the connector thread and goes on, so a message may find the tip behind the
 * blocks its peer sent first.
 *
 * Every message type has a row in MESSAGE_TIP_RULES, with the reason for its
 * side. PeerManagerImpl::ProcessMessage() asks the table once, before the
 * message's handler runs, and makes the wait there
 * (PeerManagerImpl::WaitForPendingBlocks()); no handler waits by itself.
 *
 * Two things are judged against the tip without a message of their own, and
 * wait where they are taken up: an orphan considered again
 * (PeerManagerImpl::ProcessOrphanTx()), and a block request kept from an
 * earlier pass, which activates for itself (PeerManagerImpl::ProcessGetBlockData()).
 */
enum class TipRule : uint8_t {
    //! The handler reads nothing that a block moves.
    Nothing,
    //! The handler reads what a block moves, and a tip that lags the blocks accepted is as good to it
    //! as a block that arrives a moment later; or it catches up by a means of its own, named in the row.
    TipLagTolerated,
    //! The blocks accepted before the message are connected before its handler runs. Out of initial
    //! block download only (node/sync_mode.h).
    PendingBlocksConnected,
};

struct MessageTipRule {
    std::string_view type;
    TipRule rule;
};

// clang-format off
inline constexpr MessageTipRule MESSAGE_TIP_RULES[]{
    {NetMsgType::VERSION,      TipRule::Nothing},                // the handshake; the height in it is the peer's
    {NetMsgType::VERACK,       TipRule::Nothing},                // the handshake
    {NetMsgType::ADDR,         TipRule::Nothing},                // addresses
    {NetMsgType::ADDRV2,       TipRule::Nothing},                // addresses
    {NetMsgType::SENDADDRV2,   TipRule::Nothing},                // a setting of the peer
    {NetMsgType::INV,          TipRule::PendingBlocksConnected}, // an announced transaction is dropped where the filter of recent rejects has it, and the filter is reset as the tip changes
    {NetMsgType::GETDATA,      TipRule::TipLagTolerated},        // a block request activates the chain itself, or is kept while the block is stored (ProcessGetBlockData())
    {NetMsgType::MERKLEBLOCK,  TipRule::Nothing},                // not processed by this node
    {NetMsgType::GETBLOCKS,    TipRule::TipLagTolerated},        // the handler activates the chain itself before it answers
    {NetMsgType::GETHEADERS,   TipRule::PendingBlocksConnected}, // answered from the active chain, which the peer may know to be longer from a block's announcement
    {NetMsgType::TX,           TipRule::PendingBlocksConnected}, // a transaction is judged against the tip's coins, and may spend what a block before it made
    {NetMsgType::HEADERS,      TipRule::TipLagTolerated},        // headers go to the block index; the blocks asked for on them are chosen from it and from what is in flight
    {NetMsgType::BLOCK,        TipRule::PendingBlocksConnected}, // a block finds the block before it connected, and is announced where it would have been
    {NetMsgType::GETADDR,      TipRule::Nothing},                // addresses
    {NetMsgType::MEMPOOL,      TipRule::TipLagTolerated},        // answered from the mempool as it is
    {NetMsgType::PING,         TipRule::TipLagTolerated},        // the handler does not wait: the pong is put off until the peer's blocks are connected (Peer::m_pending_pong)
    {NetMsgType::PONG,         TipRule::Nothing},                // a time measurement
    {NetMsgType::NOTFOUND,     TipRule::Nothing},                // requests in flight
    {NetMsgType::FILTERLOAD,   TipRule::Nothing},                // a setting of the peer
    {NetMsgType::FILTERADD,    TipRule::Nothing},                // a setting of the peer
    {NetMsgType::FILTERCLEAR,  TipRule::Nothing},                // a setting of the peer
    {NetMsgType::SENDHEADERS,  TipRule::Nothing},                // a setting of the peer
    {NetMsgType::FEEFILTER,    TipRule::Nothing},                // a setting of the peer
    {NetMsgType::SENDCMPCT,    TipRule::Nothing},                // a setting of the peer
    {NetMsgType::CMPCTBLOCK,   TipRule::PendingBlocksConnected}, // taken as a compact block only where it is at most two above the tip
    {NetMsgType::GETBLOCKTXN,  TipRule::TipLagTolerated},        // answered from the block asked for; its depth below a lagging tip is at most smaller
    {NetMsgType::BLOCKTXN,     TipRule::PendingBlocksConnected}, // completes a block, which is processed as one that arrives whole
    {NetMsgType::GETCFILTERS,  TipRule::TipLagTolerated},        // answered from the filter index, which follows the tip by itself
    {NetMsgType::CFILTER,      TipRule::Nothing},                // not processed by this node
    {NetMsgType::GETCFHEADERS, TipRule::TipLagTolerated},        // answered from the filter index, which follows the tip by itself
    {NetMsgType::CFHEADERS,    TipRule::Nothing},                // not processed by this node
    {NetMsgType::GETCFCHECKPT, TipRule::TipLagTolerated},        // answered from the filter index, which follows the tip by itself
    {NetMsgType::CFCHECKPT,    TipRule::Nothing},                // not processed by this node
    {NetMsgType::WTXIDRELAY,   TipRule::Nothing},                // the handshake
    {NetMsgType::SENDTXRCNCL,  TipRule::Nothing},                // the handshake
    {NetMsgType::FEATURE,      TipRule::Nothing},                // the handshake
};
// clang-format on

//! One row per known message type. That each type has one, and none has two, is message_tip_rule_tests'.
static_assert(std::size(MESSAGE_TIP_RULES) == std::tuple_size_v<decltype(ALL_NET_MESSAGE_TYPES)>);

/** The row of a message type; nothing for a type this node does not know, which it ignores. */
constexpr std::optional<TipRule> TipRuleOf(std::string_view type)
{
    for (const MessageTipRule& row : MESSAGE_TIP_RULES) {
        if (row.type == type) return row.rule;
    }
    return std::nullopt;
}
} // namespace node

#endif // BITCOIN_NODE_MESSAGE_TIP_RULE_H
