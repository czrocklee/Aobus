// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    struct SpyTrackSourceRemovals final : TrackSourceObserver
    {
      std::vector<std::pair<TrackId, std::size_t>> removed{};

      void handleReset() override {}
      void handleInserted(TrackId /*id*/, std::size_t /*index*/) override {}
      void handleUpdated(TrackId /*id*/, std::size_t /*index*/) override {}
      void handleRemoved(TrackId id, std::size_t index) override { removed.emplace_back(id, index); }
    };
  } // namespace

  TEST_CASE("TrackSourceCache - source lookup and list refresh maintain source state",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};

    auto cache = TrackSourceCache{libraryFixture.library(), changes};

    SECTION("sourceFor kInvalidListId returns allTracks")
    {
      auto& source = cache.sourceFor(kInvalidListId);
      CHECK(&source == &cache.allTracks());
    }

    SECTION("sourceFor creates manual list source")
    {
      auto listId = ListId{0};
      {
        auto transaction = libraryFixture.library().writeTransaction();
        auto builder = ListBuilder::makeEmpty();
        builder.name("ManualList");
        listId =
          ao::test::requireValue(libraryFixture.library().lists().writer(transaction).create(builder.serialize()))
            .first;
        REQUIRE(transaction.commit());
      }

      auto& source = cache.sourceFor(listId);
      CHECK(dynamic_cast<ManualListSource*>(&source) != nullptr);

      // Subsequent calls should return the same instance
      auto& source2 = cache.sourceFor(listId);
      CHECK(&source == &source2);
    }

    SECTION("sourceFor creates smart list source")
    {
      auto listId = ListId{0};
      {
        auto transaction = libraryFixture.library().writeTransaction();
        auto builder = ListBuilder::makeEmpty();
        builder.name("SmartList");
        builder.filter("title == \"foo\"");
        listId =
          ao::test::requireValue(libraryFixture.library().lists().writer(transaction).create(builder.serialize()))
            .first;
        REQUIRE(transaction.commit());
      }

      auto& source = cache.sourceFor(listId);
      CHECK(dynamic_cast<SmartListSource*>(&source) != nullptr);
    }

    SECTION("sourceFor missing list returns allTracks")
    {
      auto& source = cache.sourceFor(ListId{999});
      CHECK(&source == &cache.allTracks());
    }

    SECTION("reloadAllTracks updates allTracks source")
    {
      libraryFixture.addTrack("Track 1");
      libraryFixture.addTrack("Track 2");

      cache.reloadAllTracks();
      CHECK(cache.allTracks().size() == 2);
    }

    SECTION("track delete notifications remove allTracks membership")
    {
      auto const trackId = libraryFixture.addTrack("Track 1");
      cache.reloadAllTracks();
      REQUIRE(cache.allTracks().size() == 1);
      auto spy = SpyTrackSourceRemovals{};
      cache.allTracks().attach(&spy);

      CHECK(writer.deleteTrack(trackId).has_value());
      CHECK(cache.allTracks().size() == 0);
      CHECK(spy.removed.size() == 1);

      if (!spy.removed.empty())
      {
        CHECK(spy.removed[0] == std::pair{trackId, std::size_t{0}});
      }

      cache.allTracks().detach(&spy);
    }

    SECTION("refreshList updates manual list source")
    {
      auto listId = ListId{0};
      {
        auto transaction = libraryFixture.library().writeTransaction();
        auto builder = ListBuilder::makeEmpty();
        builder.name("Manual");
        listId =
          ao::test::requireValue(libraryFixture.library().lists().writer(transaction).create(builder.serialize()))
            .first;
        REQUIRE(transaction.commit());
      }

      auto& source = cache.sourceFor(listId);
      CHECK(source.size() == 0);

      auto t1 = libraryFixture.addTrack("A");
      cache.reloadAllTracks(); // ensure parent source has it

      {
        auto transaction = libraryFixture.library().writeTransaction();
        auto optView = libraryFixture.library().lists().reader(transaction).get(listId);
        auto builder = ListBuilder::fromView(*optView);
        builder.tracks().add(t1);
        CHECK(libraryFixture.library().lists().writer(transaction).update(listId, builder.serialize()));
        REQUIRE(transaction.commit());
      }

      cache.refreshList(listId);
      REQUIRE(source.size() == 1);
      REQUIRE(source.trackIdAt(0) == t1);
    }

    SECTION("refreshList updates smart list source")
    {
      auto listId = ListId{0};
      {
        auto transaction = libraryFixture.library().writeTransaction();
        auto builder = ListBuilder::makeEmpty();
        builder.name("Smart");
        builder.filter("$year >= 2020");
        listId =
          ao::test::requireValue(libraryFixture.library().lists().writer(transaction).create(builder.serialize()))
            .first;
        REQUIRE(transaction.commit());
      }

      auto& source = cache.sourceFor(listId);
      CHECK(source.size() == 0);

      libraryFixture.addTrack("B");
      libraryFixture.addTrack("A");

      cache.reloadAllTracks();

      cache.refreshList(listId);
      CHECK(source.size() == 2);
    }

    SECTION("refreshList invalid ID returns early")
    {
      // shouldn't crash
      cache.refreshList(kInvalidListId);
    }

    SECTION("refreshList for missing ID calls eraseList")
    {
      auto listId = ListId{0};
      {
        auto transaction = libraryFixture.library().writeTransaction();
        auto builder = ListBuilder::makeEmpty();
        builder.name("DeleteMe");
        listId =
          ao::test::requireValue(libraryFixture.library().lists().writer(transaction).create(builder.serialize()))
            .first;
        REQUIRE(transaction.commit());
      }

      cache.sourceFor(listId);

      {
        auto transaction = libraryFixture.library().writeTransaction();
        libraryFixture.library().lists().writer(transaction).remove(listId);
        REQUIRE(transaction.commit());
      }

      // refreshList should detect the missing list and erase it from _sources
      cache.refreshList(listId);

      // now sourceFor should fallback to allTracks because it was erased
      auto& fallbackSource = cache.sourceFor(listId);
      CHECK(&fallbackSource == &cache.allTracks());
    }

    SECTION("eraseList removes list and its children")
    {
      auto parentId = ListId{0};
      auto childId = ListId{0};
      auto grandchildId = ListId{0};

      {
        auto transaction = libraryFixture.library().writeTransaction();
        auto listWriter = libraryFixture.library().lists().writer(transaction);

        auto parentBuilder = ListBuilder::makeEmpty();
        parentBuilder.name("Parent");
        parentId = ao::test::requireValue(listWriter.create(parentBuilder.serialize())).first;

        auto childBuilder = ListBuilder::makeEmpty();
        childBuilder.name("Child");
        childBuilder.parentId(parentId);
        childId = ao::test::requireValue(listWriter.create(childBuilder.serialize())).first;

        auto grandchildBuilder = ListBuilder::makeEmpty();
        grandchildBuilder.name("Grandchild");
        grandchildBuilder.parentId(childId);
        grandchildId = ao::test::requireValue(listWriter.create(grandchildBuilder.serialize())).first;

        REQUIRE(transaction.commit());
      }

      // Build the hierarchy in TrackSourceCache
      cache.sourceFor(parentId);
      cache.sourceFor(childId);
      cache.sourceFor(grandchildId);

      {
        auto transaction = libraryFixture.library().writeTransaction();
        libraryFixture.library().lists().writer(transaction).remove(grandchildId);
        libraryFixture.library().lists().writer(transaction).remove(childId);
        libraryFixture.library().lists().writer(transaction).remove(parentId);
        REQUIRE(transaction.commit());
      }

      cache.eraseList(parentId);

      // All three should fallback to allTracks
      CHECK(&cache.sourceFor(parentId) == &cache.allTracks());
      CHECK(&cache.sourceFor(childId) == &cache.allTracks());
      CHECK(&cache.sourceFor(grandchildId) == &cache.allTracks());
    }

    SECTION("LibraryWriter integration")
    {
      auto listId = ListId{0};
      {
        auto transaction = libraryFixture.library().writeTransaction();
        auto builder = ListBuilder::makeEmpty();
        builder.name("ToErase");
        listId =
          ao::test::requireValue(libraryFixture.library().lists().writer(transaction).create(builder.serialize()))
            .first;
        REQUIRE(transaction.commit());
      }

      cache.sourceFor(listId);

      REQUIRE(writer.deleteList(listId));

      CHECK(&cache.sourceFor(listId) == &cache.allTracks());
    }
  }
} // namespace ao::rt::test
