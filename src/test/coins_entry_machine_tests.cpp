// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins_entry_machine.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <set>
#include <vector>

using namespace coins_entry;

namespace {
constexpr State Go(State from, Event event) { return Next(from, event); }

std::vector<Event> AllEvents()
{
    std::vector<Event> events;
    for (size_t e{0}; e < EVENT_COUNT; ++e) events.push_back(static_cast<Event>(e));
    return events;
}
} // namespace

BOOST_AUTO_TEST_SUITE(coins_entry_machine_tests)

BOOST_AUTO_TEST_CASE(known_states)
{
    // The states an entry may rest in, written out: a change of IsStored() shows here.
    const std::set<State> stored{
        BASE_COPY,
        DIRTY,
        DIRTY | FRESH,
        DIRTY | COLLECTED,
        DIRTY | BLOCK_CREATE,
        DIRTY | BLOCK_CREATE | FRESH,
        DIRTY | BLOCK_CREATE | COLLECTED,
        DIRTY | BLOCK_CREATE | OWED,
        DIRTY | BLOCK_CREATE | OWED | COLLECTED,
        DIRTY | SPENT,
        DIRTY | SPENT | COLLECTED,
        DIRTY | SPENT | BLOCK_SPEND,
        DIRTY | SPENT | BLOCK_SPEND | COLLECTED,
        DIRTY | SPENT | BLOCK_SPEND | UNWRITTEN,
    };
    for (size_t s{0}; s < 256; ++s) {
        const State state{static_cast<State>(s)};
        BOOST_CHECK_EQUAL(IsStored(state), stored.contains(state));
        BOOST_CHECK_EQUAL(IsKnown(state), stored.contains(state) || state == ABSENT);
    }
    BOOST_CHECK(!IsStored(ABSENT));
    BOOST_CHECK(!IsKnown(REFUSED));
}

BOOST_AUTO_TEST_CASE(table_is_closed_and_refuses_the_unknown)
{
    for (const Event event : AllEvents()) {
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            const State to{Go(from, event)};
            if (!IsKnown(from)) {
                BOOST_CHECK_EQUAL(to, REFUSED);
            } else {
                BOOST_CHECK(to == REFUSED || IsKnown(to));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(every_known_state_is_reached)
{
    // No state is allowed that nothing leads to: the set is what the events make of an inserted entry.
    std::array<bool, 256> seen{};
    std::vector<State> todo{ABSENT};
    seen[ABSENT] = true;
    while (!todo.empty()) {
        const State from{todo.back()};
        todo.pop_back();
        const auto add{[&](State to) {
            if (to == REFUSED || seen[to]) return;
            seen[to] = true;
            todo.push_back(to);
        }};
        for (const Event event : AllEvents()) add(Go(from, event));
        for (size_t r{0}; r < 256; ++r) add(NextUnwindSpendRestore(from, static_cast<State>(r)));
    }
    for (size_t s{0}; s < 256; ++s) BOOST_CHECK_EQUAL(seen[s], IsKnown(static_cast<State>(s)));
}

BOOST_AUTO_TEST_CASE(insertions)
{
    // An inserted entry takes exactly the events that bring something in.
    const std::set<Event> inserting{Event::BaseCopy, Event::ConfirmedNew, Event::ConfirmedWrite, Event::ConfirmedSpend, Event::BlockCreateFresh, Event::BlockCreateBlind, Event::BlockSpendMarker};
    for (const Event event : AllEvents()) {
        const State to{Go(ABSENT, event)};
        // What clears a bit an inserted entry does not have leaves it as it is: harmless, and not an insertion.
        if (to == ABSENT) continue;
        BOOST_CHECK_EQUAL(to != REFUSED, inserting.contains(event));
    }
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::BaseCopy), BASE_COPY);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::ConfirmedNew), DIRTY | FRESH);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::ConfirmedWrite), DIRTY);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::ConfirmedSpend), DIRTY | SPENT);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::BlockCreateFresh), DIRTY | FRESH | BLOCK_CREATE);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::BlockCreateBlind), DIRTY | BLOCK_CREATE);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::BlockSpendMarker), DIRTY | BLOCK_SPEND | SPENT);
    // And only an inserted entry takes those that say the tier held nothing.
    for (size_t s{0}; s < 256; ++s) {
        const State from{static_cast<State>(s)};
        if (from == ABSENT) continue;
        for (const Event event : {Event::BaseCopy, Event::ConfirmedNew, Event::BlockCreateFresh, Event::BlockCreateBlind, Event::BlockSpendMarker}) {
            BOOST_CHECK_EQUAL(Go(from, event), REFUSED);
        }
    }
}

BOOST_AUTO_TEST_CASE(fresh_coin_of_a_block)
{
    // A block creates a coin the database cannot hold, and a block spends it.
    const State created{Go(ABSENT, Event::BlockCreateFresh)};
    // No run takes a fresh coin's spend for an erase: the marker says the database never held the coin.
    const State marker{Go(created, Event::BlockSpend)};
    BOOST_CHECK_EQUAL(marker, DIRTY | BLOCK_SPEND | SPENT | UNWRITTEN);
    BOOST_CHECK_EQUAL(Go(marker, Event::RunCollects), REFUSED);
    // A lower block that takes the spend over takes what it says of the database with it.
    BOOST_CHECK_EQUAL(Go(marker, Event::BlockSpendTakeover), marker);
    // A run that wrote the coin from the block's record makes the spend an erase to write.
    const State erase{Go(marker, Event::RunWritesRecord)};
    BOOST_CHECK_EQUAL(erase, DIRTY | BLOCK_SPEND | SPENT);
    BOOST_CHECK_EQUAL(Go(erase, Event::RunCollects), erase | COLLECTED);
    // The erase is in the database: nothing is left, and the entry is erased.
    BOOST_CHECK_EQUAL(Go(erase | COLLECTED, Event::RunLands), ABSENT);
    // The unwind of the spend puts the coin back as the record has it.
    BOOST_CHECK_EQUAL(NextUnwindSpendRestore(marker, created), created);
    BOOST_CHECK_EQUAL(NextUnwindSpendRestore(ABSENT, created), created);
    BOOST_CHECK_EQUAL(NextUnwindSpendRestore(marker, BASE_COPY), BASE_COPY);
}

BOOST_AUTO_TEST_CASE(fresh_means_nothing_to_erase)
{
    // A confirmed spend of a fresh entry is no event: the entry is erased, there being no erase to write.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::ConfirmedSpend), REFUSED);
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH | BLOCK_CREATE, Event::ConfirmedSpend), REFUSED);
    // A collected entry is not fresh: a spend while the run is under way must produce an erase.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::RunCollects), DIRTY | COLLECTED);
    BOOST_CHECK_EQUAL(Go(DIRTY | COLLECTED, Event::ConfirmedSpend), DIRTY | SPENT);
    // A child's batch over an entry keeps what is known of the database, a plain overwrite does not.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::BatchOverwrite), DIRTY | FRESH);
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::ConfirmedWrite), DIRTY);
    // A coin over a spend not yet written is not fresh: the database may hold the coin that was spent.
    BOOST_CHECK_EQUAL(Go(DIRTY | SPENT, Event::ConfirmedNew), REFUSED);
    BOOST_CHECK_EQUAL(Go(DIRTY | SPENT, Event::ConfirmedWrite), DIRTY);
    // A restored coin a run wrote is in the database.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::KeptWritten), DIRTY);
    // A lower block's create that may overwrite takes the entry of a higher block's over as not fresh.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH | BLOCK_CREATE, Event::BlockCreateTakeover), DIRTY | FRESH | BLOCK_CREATE);
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH | BLOCK_CREATE, Event::BlockCreateTakeoverBlind), DIRTY | BLOCK_CREATE);
}

BOOST_AUTO_TEST_CASE(owed_under_a_create)
{
    // A block's create over a spend not yet written owes the erase; over a clean coin it owes nothing.
    const State owing{Go(DIRTY | SPENT, Event::BlockCreateOnConfirmed)};
    BOOST_CHECK_EQUAL(owing, DIRTY | BLOCK_CREATE | OWED);
    BOOST_CHECK_EQUAL(Go(DIRTY, Event::BlockCreateOnConfirmed), owing);
    BOOST_CHECK_EQUAL(Go(BASE_COPY, Event::BlockCreateOnConfirmed), DIRTY | BLOCK_CREATE);
    // A run writes what is owed and leaves the create; a takeover of the create keeps what is owed.
    BOOST_CHECK_EQUAL(Go(owing, Event::RunWritesOwed), DIRTY | BLOCK_CREATE);
    BOOST_CHECK_EQUAL(Go(DIRTY | BLOCK_CREATE, Event::RunWritesOwed), REFUSED);
    BOOST_CHECK_EQUAL(Go(owing, Event::BlockCreateTakeover), owing);
    // The unwind of the create: the erase is owed still, or the dirty coin it overwrote is put back.
    BOOST_CHECK_EQUAL(Go(owing, Event::UnwindCreateToErase), DIRTY | SPENT);
    BOOST_CHECK_EQUAL(Go(DIRTY | BLOCK_CREATE, Event::UnwindCreateToErase), REFUSED);
    BOOST_CHECK_EQUAL(Go(owing, Event::UnwindCreateToCoin), DIRTY);
    BOOST_CHECK_EQUAL(Go(DIRTY | BLOCK_CREATE, Event::UnwindCreateToCoin), DIRTY);
    // A spend of the created coin takes what is owed into the block's record, and the unwind brings it back.
    BOOST_CHECK_EQUAL(Go(owing, Event::BlockSpend), DIRTY | BLOCK_SPEND | SPENT);
    BOOST_CHECK_EQUAL(NextUnwindSpendRestore(DIRTY | BLOCK_SPEND | SPENT, owing), owing);
    BOOST_CHECK_EQUAL(Go(DIRTY | BLOCK_SPEND | SPENT, Event::UnwindSpendToErase), DIRTY | SPENT);
    // Once confirmed and within a run's bound the entry is collected with what it owed, and lands clean.
    BOOST_CHECK_EQUAL(Go(owing, Event::RunCollects), owing | COLLECTED);
    BOOST_CHECK_EQUAL(Go(owing | COLLECTED, Event::RunLands), BASE_COPY);
}

BOOST_AUTO_TEST_CASE(a_change_withdraws_the_entry_from_its_run)
{
    // Every event clears COLLECTED but those of a run on the entry it collected (which leave the value
    // the run took as it is), and the one that says a restored coin was written.
    const std::set<Event> keeping{Event::RunWritesOwed, Event::RunWritesRecord, Event::KeptWritten};
    for (const Event event : AllEvents()) {
        if (event == Event::RunCollects) continue;
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            if (!IsKnown(from) || !(from & COLLECTED)) continue;
            const State to{Go(from, event)};
            if (to == REFUSED) continue;
            BOOST_CHECK_EQUAL(static_cast<bool>(to & COLLECTED), keeping.contains(event));
        }
    }
    // Only a collected entry lands.
    for (size_t s{0}; s < 256; ++s) {
        const State from{static_cast<State>(s)};
        if (!IsKnown(from)) continue;
        BOOST_CHECK_EQUAL(Go(from, Event::RunLands) != REFUSED, static_cast<bool>(from & COLLECTED));
        if (Go(from, Event::RunLands) != REFUSED) BOOST_CHECK(!(Go(from, Event::RunLands) & DIRTY));
    }
}

BOOST_AUTO_TEST_CASE(what_a_lookup_sees)
{
    // Two bits and the coin decide it; what a run or the database is owed changes nothing for a viewer.
    for (size_t s{0}; s < 256; ++s) {
        const State state{static_cast<State>(s)};
        if (!IsKnown(state)) continue;
        const State other{static_cast<State>(state & ~(FRESH | COLLECTED | OWED | UNWRITTEN))};
        BOOST_CHECK(ReadingOf(state) == ReadingOf(other));
    }
    BOOST_CHECK(ReadingOf(ABSENT) == Reading::NOTHING);
    BOOST_CHECK(ReadingOf(BASE_COPY) == Reading::COIN);
    BOOST_CHECK(ReadingOf(DIRTY | FRESH) == Reading::COIN);
    BOOST_CHECK(ReadingOf(DIRTY | SPENT) == Reading::NOTHING);
    BOOST_CHECK(ReadingOf(DIRTY | BLOCK_CREATE | OWED) == Reading::BLOCK_COIN);
    BOOST_CHECK(ReadingOf(DIRTY | BLOCK_SPEND | SPENT | UNWRITTEN) == Reading::BLOCK_RECORD);
    // A run changes nothing a viewer sees, until the entry lands: then the database has it.
    for (size_t s{0}; s < 256; ++s) {
        const State state{static_cast<State>(s)};
        for (const Event event : {Event::RunCollects, Event::RunWritesOwed, Event::RunWritesRecord, Event::KeptWritten, Event::Flush}) {
            if (const State to{Next(state, event)}; to != REFUSED) BOOST_CHECK(ReadingOf(to) == ReadingOf(state));
        }
    }
}

namespace {
//! An entry as far as the machine knows it: its state and the height it is filed under.
struct Filed {
    State state;
    int height;
};
//! `event` at `height` happens to `entry`.
Filed Step(Filed entry, Event event, int height)
{
    const State to{Next(entry.state, event)};
    BOOST_REQUIRE(to != REFUSED);
    const auto filed{NextHeight(HeightRule(entry.state, event), entry.state, entry.height, height)};
    BOOST_REQUIRE(filed.has_value());
    return {to, *filed};
}
} // namespace

BOOST_AUTO_TEST_CASE(what_each_height_rule_does)
{
    BOOST_CHECK(NextHeight(Height::Keep, DIRTY, 5, 9) == 5);
    BOOST_CHECK(NextHeight(Height::Take, DIRTY, 5, 9) == 9);
    BOOST_CHECK(NextHeight(Height::Take, DIRTY, 9, 5) == 5);
    // Lower is Take for an entry that owes nothing, upwards as well.
    BOOST_CHECK(NextHeight(Height::Lower, DIRTY, 5, 9) == 9);
    BOOST_CHECK(NextHeight(Height::Lower, DIRTY | BLOCK_CREATE | OWED, 5, 9) == 5);
    BOOST_CHECK(NextHeight(Height::Lower, DIRTY | BLOCK_CREATE | OWED, 9, 5) == 5);
    // A height is put back at or below where the entry is filed, or for an entry inserted again.
    BOOST_CHECK(NextHeight(Height::Restore, DIRTY | SPENT | BLOCK_SPEND, 9, 5) == 5);
    BOOST_CHECK(NextHeight(Height::Restore, DIRTY | SPENT | BLOCK_SPEND, 9, 9) == 9);
    BOOST_CHECK(!NextHeight(Height::Restore, DIRTY | SPENT | BLOCK_SPEND, 5, 9));
    BOOST_CHECK(NextHeight(Height::Restore, ABSENT, 0, 9) == 9);
    BOOST_CHECK(UNWIND_SPEND_RESTORE_HEIGHT == Height::Restore);
}

BOOST_AUTO_TEST_CASE(height_rules_follow_the_table)
{
    for (const Event event : AllEvents()) {
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            // No rule, no change of height.
            if (Next(from, event) == REFUSED) BOOST_CHECK(HeightRule(from, event) == Height::Keep);
            if (!IsKnown(from) || Next(from, event) == REFUSED || !(from & OWED)) continue;
            // An entry that owes an earlier change leaves the height of that change on two grounds only:
            // a run wrote the change, or an open block spent the entry and keeps the height in its record.
            const bool takes{HeightRule(from, event) == Height::Take};
            BOOST_CHECK_EQUAL(takes, event == Event::RunWritesOwed || event == Event::BlockSpend);
        }
    }
    // What is inserted is filed under the event's height.
    for (const Event event : {Event::BaseCopy, Event::ConfirmedNew, Event::ConfirmedWrite, Event::ConfirmedSpend, Event::BlockCreateFresh, Event::BlockCreateBlind, Event::BlockSpendMarker}) {
        BOOST_CHECK(NextHeight(HeightRule(ABSENT, event), ABSENT, 0, 7) == 7);
    }
    // A run that collects or lands, and a flush, move nothing.
    for (const Event event : {Event::RunCollects, Event::RunLands, Event::RunWritesRecord, Event::KeptWritten, Event::Flush}) {
        for (size_t s{0}; s < 256; ++s) BOOST_CHECK(HeightRule(static_cast<State>(s), event) == Height::Keep);
    }
}

BOOST_AUTO_TEST_CASE(owed_erase_keeps_its_height)
{
    // A confirmed spend at 2 of a coin the database holds: the erase is owed since 2.
    const Filed erase{Step({BASE_COPY, 1}, Event::ConfirmedSpend, 2)};
    BOOST_CHECK_EQUAL(erase.state, DIRTY | SPENT);
    BOOST_CHECK_EQUAL(erase.height, 2);

    // An open block makes the outpoint again at 4: the erase is owed under the create.
    const Filed created{Step(erase, Event::BlockCreateOnConfirmed, 4)};
    BOOST_CHECK_EQUAL(created.state, DIRTY | BLOCK_CREATE | OWED);
    BOOST_CHECK_EQUAL(created.height, 2);
    // A lower block taking the create over, and confirmed changes once the block is confirmed, leave it there.
    BOOST_CHECK_EQUAL(Step(created, Event::BlockCreateTakeover, 3).height, 2);
    BOOST_CHECK_EQUAL(Step(created, Event::ConfirmedSpend, 6).height, 2);
    BOOST_CHECK_EQUAL(Step(created, Event::ConfirmedWrite, 6).height, 2);
    BOOST_CHECK_EQUAL(Step(created, Event::BatchOverwrite, 6).height, 2);
    // The create's unwind leaves the erase where it was.
    BOOST_CHECK_EQUAL(Step(created, Event::UnwindCreateToErase, 0).height, 2);
    // A run that writes the erase files the entry under its coin.
    const Filed written{Step(created, Event::RunWritesOwed, 4)};
    BOOST_CHECK_EQUAL(written.state, DIRTY | BLOCK_CREATE);
    BOOST_CHECK_EQUAL(written.height, 4);

    // A block's spend at 5 takes the entry under the block's height, and its confirmation gives it back:
    // as the marker,
    const Filed marker{Step(created, Event::BlockSpend, 5)};
    BOOST_CHECK_EQUAL(marker.state, DIRTY | SPENT | BLOCK_SPEND);
    BOOST_CHECK_EQUAL(marker.height, 5);
    BOOST_CHECK_EQUAL(Step(marker, Event::BlockSpendConfirmed, 2).height, 2);
    // as a higher block's create over the confirmed marker,
    const Filed again{Step(marker, Event::BlockCreateOnConfirmed, 8)};
    BOOST_CHECK_EQUAL(again.state, DIRTY | BLOCK_CREATE | OWED);
    BOOST_CHECK_EQUAL(again.height, 5);
    BOOST_CHECK_EQUAL(Step(again, Event::BlockSpendConfirmed, 2).height, 2);
    // and as what that create's unwind left.
    const Filed unwound{Step(again, Event::UnwindCreateToErase, 0)};
    BOOST_CHECK_EQUAL(unwound.state, DIRTY | SPENT);
    BOOST_CHECK_EQUAL(Step(unwound, Event::BlockSpendConfirmed, 2).height, 2);
    // The spend's unwind gives it back as well.
    BOOST_CHECK_EQUAL(Step(marker, Event::UnwindSpendToErase, 2).height, 2);
    // A run that collected the entry under the block's height writes the erase itself.
    BOOST_CHECK_EQUAL(Next(Step(marker, Event::RunCollects, 0).state, Event::BlockSpendConfirmed), REFUSED);

    // A dirty coin under an open create (a coinbase made twice) goes back to its height with the unwind,
    // also after a run wrote it and filed the entry under the create.
    const Filed coin{Step({ABSENT, 0}, Event::ConfirmedWrite, 3)};
    const Filed over{Step(coin, Event::BlockCreateOnConfirmed, 6)};
    BOOST_CHECK_EQUAL(over.height, 3);
    BOOST_CHECK_EQUAL(Step(over, Event::UnwindCreateToCoin, 3).height, 3);
    BOOST_CHECK_EQUAL(Step(Step(over, Event::RunWritesOwed, 6), Event::UnwindCreateToCoin, 3).height, 3);
}

BOOST_AUTO_TEST_CASE(restore_takes_a_marker_or_nothing)
{
    for (size_t s{0}; s < 256; ++s) {
        const State from{static_cast<State>(s)};
        const bool marker{IsStored(from) && (from & BLOCK_SPEND)};
        BOOST_CHECK_EQUAL(NextUnwindSpendRestore(from, DIRTY) != REFUSED, marker || from == ABSENT);
    }
    // What is put back is a coin as a block could have spent it: none a run holds, no spend.
    for (size_t r{0}; r < 256; ++r) {
        const State recorded{static_cast<State>(r)};
        const bool coin{IsStored(recorded) && !(recorded & (SPENT | COLLECTED))};
        BOOST_CHECK_EQUAL(NextUnwindSpendRestore(ABSENT, recorded) != REFUSED, coin);
        if (coin) BOOST_CHECK_EQUAL(NextUnwindSpendRestore(ABSENT, recorded), recorded);
    }
}

BOOST_AUTO_TEST_SUITE_END()
