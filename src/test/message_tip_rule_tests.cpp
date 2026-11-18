// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/message_tip_rule.h>

#include <protocol.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <set>
#include <string>

using node::TipRule;

BOOST_AUTO_TEST_SUITE(message_tip_rule_tests)

//! A message type without a row would be processed without the question having been asked.
BOOST_AUTO_TEST_CASE(every_message_type_has_one_row)
{
    for (const std::string& type : ALL_NET_MESSAGE_TYPES) {
        const auto rows{std::ranges::count_if(node::MESSAGE_TIP_RULES, [&](const auto& row) { return row.type == type; })};
        BOOST_CHECK_MESSAGE(rows == 1, type << " has " << rows << " rows");
        BOOST_CHECK(node::TipRuleOf(type).has_value());
    }
    for (const node::MessageTipRule& row : node::MESSAGE_TIP_RULES) {
        BOOST_CHECK_MESSAGE(std::ranges::count(ALL_NET_MESSAGE_TYPES, std::string{row.type}) == 1, row.type << " is no message type");
    }
    // A type the node does not know is ignored, and waits for nothing.
    BOOST_CHECK(!node::TipRuleOf("nosuchmessage"));
    BOOST_CHECK(!node::TipRuleOf(""));
}

//! The messages the handler connects the pending blocks for. A row that changes sides changes what the
//! node does, and is to change this list with it.
BOOST_AUTO_TEST_CASE(messages_judged_against_the_tip)
{
    std::set<std::string> waits;
    for (const node::MessageTipRule& row : node::MESSAGE_TIP_RULES) {
        if (row.rule == TipRule::PendingBlocksConnected) waits.emplace(row.type);
    }
    const std::set<std::string> expected{NetMsgType::INV, NetMsgType::GETHEADERS, NetMsgType::TX, NetMsgType::BLOCK, NetMsgType::CMPCTBLOCK, NetMsgType::BLOCKTXN};
    BOOST_CHECK(waits == expected);
}

BOOST_AUTO_TEST_SUITE_END()
