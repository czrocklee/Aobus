// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct ManualListWriterFixture final
    {
      ManualListWriterFixture()
        : writer{libraryFixture.library(), changes}
        , listSub{
            changes.onListsMutated([this](LibraryChanges::ListsMutated const& event) { listEvents.push_back(event); })}
      {
      }

      TrackId addTrack(std::string_view title) { return libraryFixture.addTrack(title); }

      ListId createManual(std::span<TrackId const> trackIds = {}, ListId parentId = kInvalidListId)
      {
        auto draft = LibraryWriter::ListDraft{};
        draft.kind = LibraryWriter::ListKind::Manual;
        draft.parentId = parentId;
        draft.name = "Manual";
        draft.trackIds.assign(trackIds.begin(), trackIds.end());
        auto const listId = ao::test::requireValue(writer.createList(draft));
        listEvents.clear();
        return listId;
      }

      ListId createSmart()
      {
        auto draft = LibraryWriter::ListDraft{};
        draft.kind = LibraryWriter::ListKind::Smart;
        draft.name = "Smart";
        draft.expression = "true";
        auto const listId = ao::test::requireValue(writer.createList(draft));
        listEvents.clear();
        return listId;
      }

      ListId createStoredManualUnchecked(std::span<TrackId const> trackIds)
      {
        auto transaction = libraryFixture.library().writeTransaction();
        auto builder = library::ListBuilder::makeEmpty().name("Legacy manual");

        for (auto const trackId : trackIds)
        {
          builder.tracks().add(trackId);
        }

        auto const result = libraryFixture.library().lists().writer(transaction).create(builder.serialize());
        REQUIRE(result);
        REQUIRE(transaction.commit());
        listEvents.clear();
        return result->first;
      }

      std::vector<TrackId> storedTrackIds(ListId listId)
      {
        auto transaction = libraryFixture.library().readTransaction();
        auto const optView = libraryFixture.library().lists().reader(transaction).get(listId);
        REQUIRE(optView);
        return {optView->tracks().begin(), optView->tracks().end()};
      }

      std::size_t listCount()
      {
        auto transaction = libraryFixture.library().readTransaction();
        std::size_t count = 0;

        for ([[maybe_unused]] auto const& item : libraryFixture.library().lists().reader(transaction))
        {
          ++count;
        }

        return count;
      }

      MusicLibraryFixture libraryFixture;
      LibraryChanges changes;
      LibraryWriter writer;
      std::vector<LibraryChanges::ListsMutated> listEvents;
      Subscription listSub;
    };
  } // namespace

  TEST_CASE("LibraryWriter manual lists - full drafts canonicalize and report pure reorder",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const third = fixture.addTrack("Third");
    auto const createIds = std::array{second, first, second, third, first};
    auto const listId = fixture.createManual(createIds);

    CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{second, first, third});

    auto draft = LibraryWriter::ListDraft{};
    draft.kind = LibraryWriter::ListKind::Manual;
    draft.listId = listId;
    draft.name = "Manual";
    draft.trackIds = {third, second, third, first};

    auto const result = fixture.writer.updateList(draft);

    REQUIRE(result);
    CHECK(result->changed);
    CHECK(result->trackOrderChanged);
    CHECK(result->addedTrackIds.empty());
    CHECK(result->removedTrackIds.empty());
    CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{third, second, first});
    REQUIRE(fixture.listEvents.size() == 1);
    REQUIRE(fixture.listEvents[0].manualContentChanges.size() == 1);
    CHECK(std::holds_alternative<ManualTracksReset>(fixture.listEvents[0].manualContentChanges[0].operation));
  }

  TEST_CASE("LibraryWriter manual lists - full drafts reject missing tracks atomically",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const existing = fixture.addTrack("Existing");
    auto const originalCount = fixture.listCount();
    auto draft = LibraryWriter::ListDraft{};
    draft.kind = LibraryWriter::ListKind::Manual;
    draft.name = "Invalid";
    draft.trackIds = {existing, kInvalidTrackId, TrackId{9999}};

    auto const result = fixture.writer.createList(draft);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(fixture.listCount() == originalCount);
    CHECK(fixture.listEvents.empty());
  }

  TEST_CASE("LibraryWriter manual lists - full-draft update rejects missing tracks without partial state",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const existing = fixture.addTrack("Existing");
    auto const listId = fixture.createManual(std::array{existing});
    auto draft = LibraryWriter::ListDraft{};
    draft.kind = LibraryWriter::ListKind::Manual;
    draft.listId = listId;
    draft.name = "Rejected";
    draft.trackIds = {existing, TrackId{9999}};

    auto const result = fixture.writer.updateList(draft);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{existing});
    CHECK(fixture.listEvents.empty());
  }

  TEST_CASE("LibraryWriter manual lists - metadata-only update emits no content reset",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const trackId = fixture.addTrack("Track");
    auto const listId = fixture.createManual(std::array{trackId});
    auto draft = LibraryWriter::ListDraft{};
    draft.kind = LibraryWriter::ListKind::Manual;
    draft.listId = listId;
    draft.name = "Renamed";
    draft.trackIds = {trackId};

    auto const result = fixture.writer.updateList(draft);

    REQUIRE(result);
    CHECK(result->changed);
    CHECK_FALSE(result->trackOrderChanged);
    REQUIRE(fixture.listEvents.size() == 1);
    CHECK(fixture.listEvents[0].manualContentChanges.empty());
  }

  TEST_CASE("LibraryWriter manual lists - insert supports front middle and end gaps",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const front = fixture.addTrack("Front");
    auto const middle = fixture.addTrack("Middle");
    auto const end = fixture.addTrack("End");
    auto const listId = fixture.createManual(std::array{first, second});

    auto result = fixture.writer.insertManualListTracks(listId, 0, std::array{front});
    REQUIRE(result);
    CHECK(result->insertedTrackIds == std::vector<TrackId>{front});
    CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{front, first, second});

    result = fixture.writer.insertManualListTracks(listId, 2, std::array{middle});
    REQUIRE(result);
    CHECK(result->insertionIndex == 2);
    CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{front, first, middle, second});

    result = fixture.writer.insertManualListTracks(listId, 4, std::array{end});
    REQUIRE(result);
    CHECK(result->insertionIndex == 4);
    CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{front, first, middle, second, end});
  }

  TEST_CASE("LibraryWriter manual lists - insert classification preserves reason precedence and order",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const existing = fixture.addTrack("Existing");
    auto const hidden = fixture.addTrack("Parent-hidden");
    auto const parentId = fixture.createManual(std::array{existing});
    auto const listId = fixture.createManual(std::array{existing}, parentId);
    auto const missing = TrackId{9999};
    auto const request = std::array{existing, existing, missing, missing, kInvalidTrackId, hidden};

    auto const result = fixture.writer.insertManualListTracks(listId, 1, request);

    REQUIRE(result);
    CHECK(result->changed);
    CHECK(result->insertedTrackIds == std::vector<TrackId>{hidden});
    CHECK(result->duplicateRequest == std::vector<TrackId>{existing, missing});
    CHECK(result->alreadyPresent == std::vector<TrackId>{existing});
    CHECK(result->missingTrack == std::vector<TrackId>{missing, kInvalidTrackId});
    CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{existing, hidden});
    REQUIRE(fixture.listEvents.size() == 1);
    REQUIRE(fixture.listEvents[0].manualContentChanges.size() == 1);
    auto const* operation = std::get_if<ManualTracksInsert>(&fixture.listEvents[0].manualContentChanges[0].operation);
    REQUIRE(operation != nullptr);
    CHECK(operation->storedIndex == 1);
    CHECK(operation->trackIds == std::vector<TrackId>{hidden});
  }

  TEST_CASE("LibraryWriter manual lists - all-skipped insert is a validated no-op",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const existing = fixture.addTrack("Existing");
    auto const listId = fixture.createManual(std::array{existing});

    auto const result = fixture.writer.insertManualListTracks(listId, 1, std::array{existing});

    REQUIRE(result);
    CHECK_FALSE(result->changed);
    CHECK(result->insertionIndex == 1);
    CHECK(result->alreadyPresent == std::vector<TrackId>{existing});
    CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{existing});
    CHECK(fixture.listEvents.empty());

    auto const invalid = fixture.writer.insertManualListTracks(listId, 2, std::array{existing});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == Error::Code::InvalidInput);
    CHECK(fixture.listEvents.empty());
  }

  TEST_CASE("LibraryWriter manual lists - remove reports stored order and descending exact ranges",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const third = fixture.addTrack("Third");
    auto const fourth = fixture.addTrack("Fourth");
    auto const fifth = fixture.addTrack("Fifth");
    auto const listId = fixture.createManual(std::array{first, second, third, fourth, fifth});
    auto const missing = TrackId{9999};
    auto const request = std::array{fourth, second, fourth, missing, second};

    auto const result = fixture.writer.removeManualListTracks(listId, request);

    REQUIRE(result);
    CHECK(result->changed);
    CHECK(result->removedTrackIds == std::vector<TrackId>{second, fourth});
    CHECK(result->duplicateRequest == std::vector<TrackId>{fourth, second});
    CHECK(result->notPresent == std::vector<TrackId>{missing});
    CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{first, third, fifth});
    REQUIRE(fixture.listEvents.size() == 1);
    REQUIRE(fixture.listEvents[0].manualContentChanges.size() == 1);
    auto const* operation = std::get_if<ManualTracksRemove>(&fixture.listEvents[0].manualContentChanges[0].operation);
    REQUIRE(operation != nullptr);
    CHECK(operation->removals ==
          std::vector<ManualStoredRemoveRange>{{.start = 3, .trackIds = {fourth}}, {.start = 1, .trackIds = {second}}});
  }

  TEST_CASE("LibraryWriter manual lists - move uses stored selection order and post-removal gaps",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const third = fixture.addTrack("Third");
    auto const fourth = fixture.addTrack("Fourth");
    auto const fifth = fixture.addTrack("Fifth");
    auto const listId = fixture.createManual(std::array{first, second, third, fourth, fifth});
    auto const missing = TrackId{9999};
    auto const request = std::array{fourth, second, fourth, missing};
    std::size_t insertionIndex = 0;
    auto expected = std::vector<TrackId>{};

    SECTION("front")
    {
      insertionIndex = 0;
      expected = {second, fourth, first, third, fifth};
    }

    SECTION("middle")
    {
      insertionIndex = 2;
      expected = {first, third, second, fourth, fifth};
    }

    SECTION("end")
    {
      insertionIndex = 3;
      expected = {first, third, fifth, second, fourth};
    }

    auto const result = fixture.writer.moveManualListTracks(listId, request, insertionIndex);

    REQUIRE(result);
    CHECK(result->changed);
    CHECK(result->selectedTrackIds == std::vector<TrackId>{second, fourth});
    CHECK(result->duplicateRequest == std::vector<TrackId>{fourth});
    CHECK(result->notPresent == std::vector<TrackId>{missing});
    CHECK(result->insertionIndexAfterRemoval == insertionIndex);
    CHECK(fixture.storedTrackIds(listId) == expected);
    REQUIRE(fixture.listEvents.size() == 1);
    REQUIRE(fixture.listEvents[0].manualContentChanges.size() == 1);
    auto const* operation = std::get_if<ManualTracksMove>(&fixture.listEvents[0].manualContentChanges[0].operation);
    REQUIRE(operation != nullptr);
    CHECK(operation->removals ==
          std::vector<ManualStoredRemoveRange>{{.start = 3, .trackIds = {fourth}}, {.start = 1, .trackIds = {second}}});
    CHECK(operation->insertionIndexAfterRemoval == insertionIndex);
    CHECK(operation->insertedTrackIds == std::vector<TrackId>{second, fourth});
  }

  TEST_CASE("LibraryWriter manual lists - same-order and empty-selection moves are no-ops",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const third = fixture.addTrack("Third");
    auto const fourth = fixture.addTrack("Fourth");
    auto const listId = fixture.createManual(std::array{first, second, third, fourth});

    auto result = fixture.writer.moveManualListTracks(listId, std::array{second, third}, 1);
    REQUIRE(result);
    CHECK_FALSE(result->changed);
    CHECK(result->selectedTrackIds == std::vector<TrackId>{second, third});
    CHECK(fixture.listEvents.empty());

    result = fixture.writer.moveManualListTracks(listId, std::array{TrackId{9999}}, 4);
    REQUIRE(result);
    CHECK_FALSE(result->changed);
    CHECK(result->insertionIndexAfterRemoval == 4);
    CHECK(result->notPresent == std::vector<TrackId>{TrackId{9999}});
    CHECK(fixture.listEvents.empty());

    auto const invalid = fixture.writer.moveManualListTracks(listId, std::array{TrackId{9999}}, 5);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("LibraryWriter manual lists - preview replies match commits without publication",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const third = fixture.addTrack("Third");
    auto const fourth = fixture.addTrack("Fourth");

    SECTION("insert")
    {
      auto const listId = fixture.createManual(std::array{first, second});
      auto const preview = fixture.writer.previewInsertManualListTracks(listId, 1, std::array{third});
      REQUIRE(preview);
      CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{first, second});
      CHECK(fixture.listEvents.empty());

      auto const commit = fixture.writer.insertManualListTracks(listId, 1, std::array{third});
      REQUIRE(commit);
      CHECK(*commit == *preview);
      CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{first, third, second});
      CHECK(fixture.listEvents.size() == 1);
    }

    SECTION("remove")
    {
      auto const listId = fixture.createManual(std::array{first, second, third});
      auto const preview = fixture.writer.previewRemoveManualListTracks(listId, std::array{second});
      REQUIRE(preview);
      CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{first, second, third});
      CHECK(fixture.listEvents.empty());

      auto const commit = fixture.writer.removeManualListTracks(listId, std::array{second});
      REQUIRE(commit);
      CHECK(*commit == *preview);
      CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{first, third});
      CHECK(fixture.listEvents.size() == 1);
    }

    SECTION("move")
    {
      auto const listId = fixture.createManual(std::array{first, second, third, fourth});
      auto const preview = fixture.writer.previewMoveManualListTracks(listId, std::array{second, fourth}, 0);
      REQUIRE(preview);
      CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{first, second, third, fourth});
      CHECK(fixture.listEvents.empty());

      auto const commit = fixture.writer.moveManualListTracks(listId, std::array{second, fourth}, 0);
      REQUIRE(commit);
      CHECK(*commit == *preview);
      CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{second, fourth, first, third});
      CHECK(fixture.listEvents.size() == 1);
    }
  }

  TEST_CASE("LibraryWriter manual lists - commands reject missing smart and invalid-index targets",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const trackId = fixture.addTrack("Track");

    SECTION("missing list")
    {
      auto const result = fixture.writer.insertManualListTracks(ListId{9999}, 0, std::array{trackId});
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }

    SECTION("smart list")
    {
      auto const listId = fixture.createSmart();
      auto const result = fixture.writer.removeManualListTracks(listId, std::array{trackId});
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }

    SECTION("post-removal move gap")
    {
      auto const listId = fixture.createManual(std::array{trackId});
      auto const result = fixture.writer.moveManualListTracks(listId, std::array{trackId}, 1);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }
  }

  TEST_CASE("LibraryWriter manual lists - legacy missing members remain removable and movable",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const existing = fixture.addTrack("Existing");
    auto const missing = TrackId{9999};

    SECTION("remove")
    {
      auto const listId = fixture.createStoredManualUnchecked(std::array{missing, existing});
      auto const result = fixture.writer.removeManualListTracks(listId, std::array{missing});
      REQUIRE(result);
      CHECK(result->removedTrackIds == std::vector<TrackId>{missing});
      CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{existing});
    }

    SECTION("move")
    {
      auto const listId = fixture.createStoredManualUnchecked(std::array{missing, existing});
      auto const result = fixture.writer.moveManualListTracks(listId, std::array{missing}, 1);
      REQUIRE(result);
      CHECK(result->selectedTrackIds == std::vector<TrackId>{missing});
      CHECK(fixture.storedTrackIds(listId) == std::vector<TrackId>{existing, missing});
    }
  }

  TEST_CASE("LibraryWriter manual lists - track deletion publishes exact removals for every list",
            "[runtime][unit][library][manual-list]")
  {
    auto fixture = ManualListWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const target = fixture.addTrack("Target");
    auto const third = fixture.addTrack("Third");
    auto const fourth = fixture.addTrack("Fourth");
    auto const firstList = fixture.createManual(std::array{target, first});
    auto const secondList = fixture.createManual(std::array{first, third, target, fourth});
    [[maybe_unused]] auto const smartList = fixture.createSmart();

    auto const result = fixture.writer.deleteTrack(target);

    REQUIRE(result);
    CHECK(result->removedFromListIds == std::vector<ListId>{firstList, secondList});
    CHECK(fixture.storedTrackIds(firstList) == std::vector<TrackId>{first});
    CHECK(fixture.storedTrackIds(secondList) == std::vector<TrackId>{first, third, fourth});
    REQUIRE(fixture.listEvents.size() == 1);
    CHECK(fixture.listEvents[0].upserted == std::vector<ListId>{firstList, secondList});
    REQUIRE(fixture.listEvents[0].manualContentChanges.size() == 2);

    auto const* firstRemoval =
      std::get_if<ManualTracksRemove>(&fixture.listEvents[0].manualContentChanges[0].operation);
    auto const* secondRemoval =
      std::get_if<ManualTracksRemove>(&fixture.listEvents[0].manualContentChanges[1].operation);
    REQUIRE(firstRemoval != nullptr);
    REQUIRE(secondRemoval != nullptr);
    CHECK(firstRemoval->removals == std::vector<ManualStoredRemoveRange>{{.start = 0, .trackIds = {target}}});
    CHECK(secondRemoval->removals == std::vector<ManualStoredRemoveRange>{{.start = 2, .trackIds = {target}}});
  }
} // namespace ao::rt::test
