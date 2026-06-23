// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/SmartListSource.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::rt::test
{
  using namespace ao::library;

  TEST_CASE("ListSourceStore - Basic Operations", "[runtime][unit][ListSourceStore]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto store = ListSourceStore{testLib.library(), changes};

    SECTION("sourceFor kInvalidListId returns allTracks")
    {
      auto& source = store.sourceFor(kInvalidListId);
      REQUIRE(&source == &store.allTracks());
    }

    SECTION("sourceFor creates manual list source")
    {
      auto listId = ListId{0};
      {
        auto txn = testLib.library().writeTransaction();
        auto builder = ListBuilder::createNew();
        builder.name("ManualList");
        listId = ao::test::requireValue(testLib.library().lists().writer(txn).create(builder.serialize())).first;
        txn.commit();
      }

      auto& source = store.sourceFor(listId);
      REQUIRE(dynamic_cast<ManualListSource*>(&source) != nullptr);

      // Subsequent calls should return the same instance
      auto& source2 = store.sourceFor(listId);
      REQUIRE(&source == &source2);
    }

    SECTION("sourceFor creates smart list source")
    {
      auto listId = ListId{0};
      {
        auto txn = testLib.library().writeTransaction();
        auto builder = ListBuilder::createNew();
        builder.name("SmartList");
        builder.filter("title == \"foo\"");
        listId = ao::test::requireValue(testLib.library().lists().writer(txn).create(builder.serialize())).first;
        txn.commit();
      }

      auto& source = store.sourceFor(listId);
      REQUIRE(dynamic_cast<SmartListSource*>(&source) != nullptr);
    }

    SECTION("sourceFor missing list returns allTracks")
    {
      auto& source = store.sourceFor(ListId{999});
      REQUIRE(&source == &store.allTracks());
    }

    SECTION("reloadAllTracks updates allTracks source")
    {
      testLib.addTrack("Track 1");
      testLib.addTrack("Track 2");

      store.reloadAllTracks();
      REQUIRE(store.allTracks().size() == 2);
    }

    SECTION("refreshList updates manual list source")
    {
      auto listId = ListId{0};
      {
        auto txn = testLib.library().writeTransaction();
        auto builder = ListBuilder::createNew();
        builder.name("Manual");
        listId = ao::test::requireValue(testLib.library().lists().writer(txn).create(builder.serialize())).first;
        txn.commit();
      }

      auto& source = store.sourceFor(listId);
      REQUIRE(source.size() == 0);

      auto t1 = testLib.addTrack("A");
      store.reloadAllTracks(); // ensure parent source has it

      {
        auto txn = testLib.library().writeTransaction();
        auto optView = testLib.library().lists().reader(txn).get(listId);
        auto builder = ListBuilder::fromView(*optView);
        builder.tracks().add(t1);
        testLib.library().lists().writer(txn).update(listId, builder.serialize());
        txn.commit();
      }

      store.refreshList(listId);
      REQUIRE(source.size() == 1);
      REQUIRE(source.trackIdAt(0) == t1);
    }

    SECTION("refreshList updates smart list source")
    {
      auto listId = ListId{0};
      {
        auto txn = testLib.library().writeTransaction();
        auto builder = ListBuilder::createNew();
        builder.name("Smart");
        builder.filter("$year >= 2020");
        listId = ao::test::requireValue(testLib.library().lists().writer(txn).create(builder.serialize())).first;
        txn.commit();
      }

      auto& source = store.sourceFor(listId);
      REQUIRE(source.size() == 0);

      testLib.addTrack("B");
      testLib.addTrack("A");

      store.reloadAllTracks();

      store.refreshList(listId);
      REQUIRE(source.size() == 2);
    }

    SECTION("refreshList invalid ID returns early")
    {
      // shouldn't crash
      store.refreshList(kInvalidListId);
    }

    SECTION("refreshList for missing ID calls eraseList")
    {
      auto listId = ListId{0};
      {
        auto txn = testLib.library().writeTransaction();
        auto builder = ListBuilder::createNew();
        builder.name("DeleteMe");
        listId = ao::test::requireValue(testLib.library().lists().writer(txn).create(builder.serialize())).first;
        txn.commit();
      }

      store.sourceFor(listId);

      {
        auto txn = testLib.library().writeTransaction();
        testLib.library().lists().writer(txn).del(listId);
        txn.commit();
      }

      // refreshList should detect the missing list and erase it from _sources
      store.refreshList(listId);

      // now sourceFor should fallback to allTracks because it was erased
      auto& fallbackSource = store.sourceFor(listId);
      REQUIRE(&fallbackSource == &store.allTracks());
    }

    SECTION("eraseList removes list and its children")
    {
      auto parentId = ListId{0};
      auto childId = ListId{0};
      auto grandchildId = ListId{0};

      {
        auto txn = testLib.library().writeTransaction();
        auto writer = testLib.library().lists().writer(txn);

        auto parentBuilder = ListBuilder::createNew();
        parentBuilder.name("Parent");
        parentId = ao::test::requireValue(writer.create(parentBuilder.serialize())).first;

        auto childBuilder = ListBuilder::createNew();
        childBuilder.name("Child");
        childBuilder.parentId(parentId);
        childId = ao::test::requireValue(writer.create(childBuilder.serialize())).first;

        auto grandchildBuilder = ListBuilder::createNew();
        grandchildBuilder.name("Grandchild");
        grandchildBuilder.parentId(childId);
        grandchildId = ao::test::requireValue(writer.create(grandchildBuilder.serialize())).first;

        txn.commit();
      }

      // Build the hierarchy in ListSourceStore
      store.sourceFor(parentId);
      store.sourceFor(childId);
      store.sourceFor(grandchildId);

      {
        auto txn = testLib.library().writeTransaction();
        testLib.library().lists().writer(txn).del(grandchildId);
        testLib.library().lists().writer(txn).del(childId);
        testLib.library().lists().writer(txn).del(parentId);
        txn.commit();
      }

      store.eraseList(parentId);

      // All three should fallback to allTracks
      REQUIRE(&store.sourceFor(parentId) == &store.allTracks());
      REQUIRE(&store.sourceFor(childId) == &store.allTracks());
      REQUIRE(&store.sourceFor(grandchildId) == &store.allTracks());
    }

    SECTION("LibraryWriter integration")
    {
      auto listId = ListId{0};
      {
        auto txn = testLib.library().writeTransaction();
        auto builder = ListBuilder::createNew();
        builder.name("ToErase");
        listId = ao::test::requireValue(testLib.library().lists().writer(txn).create(builder.serialize())).first;
        txn.commit();
      }

      store.sourceFor(listId);

      writer.deleteList(listId);

      REQUIRE(&store.sourceFor(listId) == &store.allTracks());
    }
  }
}
