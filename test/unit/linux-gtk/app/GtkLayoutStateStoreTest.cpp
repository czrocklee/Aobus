// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkLayoutStateStore.h"

#include "test/unit/TestFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace ao::gtk::test
{
  TEST_CASE("GtkLayoutStateStore - missing and rejected groups preserve caller state", "[gtk][unit][app][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};

    auto const seededLayouts = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{7},
       {uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 123, .weight = 2.25, .visible = false}}},
    };
    auto const seededPreferences = uimodel::ListPresentations::Snapshot{{ListId{7}, "artists"}};

    SECTION("Load non-existent config returns default")
    {
      auto const store = GtkLayoutStateStore{libraryPath};
      auto newState = seededLayouts;
      auto newPrefState = seededPreferences;
      store.load(newState, newPrefState);

      CHECK(newState == seededLayouts);
      CHECK(newPrefState == seededPreferences);
    }

    SECTION("Reject unversioned numeric column layouts without changing seeded state")
    {
      std::filesystem::create_directories(libraryPath);
      auto output = std::ofstream{libraryPath / "gtk_layout.yaml"};
      output << "trackView.columnLayouts:\n"
                "  listLayouts:\n"
                "    42:\n"
                "      - field: 0\n"
                "        width: 321\n";
      output.close();

      auto const store = GtkLayoutStateStore{libraryPath};
      auto state = seededLayouts;
      auto prefState = seededPreferences;
      store.load(state, prefState);

      CHECK(state == seededLayouts);
      CHECK(prefState == seededPreferences);
    }

    SECTION("Reject unversioned presentation preferences without changing seeded state")
    {
      std::filesystem::create_directories(libraryPath);
      auto output = std::ofstream{libraryPath / "gtk_layout.yaml"};
      output << "trackView.presentations:\n"
                "  presentations:\n"
                "    42: albums\n";
      output.close();

      auto const store = GtkLayoutStateStore{libraryPath};
      auto state = seededLayouts;
      auto prefState = seededPreferences;
      store.load(state, prefState);

      CHECK(state == seededLayouts);
      CHECK(prefState == seededPreferences);
    }
  }

  TEST_CASE("GtkLayoutStateStore - save round-trips both groups through a fresh reader", "[gtk][unit][app][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const expectedState = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{10},
       {uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = -1, .weight = 1.75, .visible = true}}},
      {ListId{20},
       {uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 200, .weight = -1.0, .visible = true}}},
    };
    auto const expectedPrefState = uimodel::ListPresentations::Snapshot{{ListId{10}, "albums"}};

    {
      auto store = GtkLayoutStateStore{libraryPath};
      store.save(expectedState, expectedPrefState);
    }

    auto const serialized = ao::test::readFile(libraryPath / "gtk_layout.yaml");
    CHECK(serialized == "trackView.columnLayouts:\n"
                        "  version: 2\n"
                        "  layouts:\n"
                        "    - listId: 10\n"
                        "      columns:\n"
                        "        - field: \"artist\"\n"
                        "          width: -1\n"
                        "          weight: 1.75\n"
                        "          visible: true\n"
                        "    - listId: 20\n"
                        "      columns:\n"
                        "        - field: \"duration\"\n"
                        "          width: 200\n"
                        "          weight: -1\n"
                        "          visible: true\n"
                        "trackView.presentations:\n"
                        "  version: 1\n"
                        "  preferences:\n"
                        "    - listId: 10\n"
                        "      presentationId: \"albums\"\n");

    auto const freshStore = GtkLayoutStateStore{libraryPath};
    auto state = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{99}, {uimodel::TrackColumnState{.field = rt::TrackField::Title, .width = 999}}},
    };
    auto prefState = uimodel::ListPresentations::Snapshot{{ListId{99}, "negative-seed"}};
    freshStore.load(state, prefState);

    CHECK(state == expectedState);
    CHECK(prefState == expectedPrefState);
  }

  TEST_CASE("GtkLayoutStateStore - unsupported column group does not block a valid presentation group",
            "[gtk][unit][app][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    std::filesystem::create_directories(libraryPath);
    auto const stored = std::string{"trackView.columnLayouts:\n"
                                    "  version: 3\n"
                                    "  layouts: []\n"
                                    "trackView.presentations:\n"
                                    "  version: 1\n"
                                    "  preferences:\n"
                                    "    - listId: 42\n"
                                    "      presentationId: albums\n"};
    std::ofstream{libraryPath / "gtk_layout.yaml"} << stored;

    auto const expectedState = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{7},
       {uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 123, .weight = -1.0, .visible = true}}},
    };
    auto const expectedPrefState = uimodel::ListPresentations::Snapshot{{ListId{42}, "albums"}};
    auto const store = GtkLayoutStateStore{libraryPath};
    auto state = expectedState;
    auto prefState = uimodel::ListPresentations::Snapshot{{ListId{7}, "artists"}};
    store.load(state, prefState);

    CHECK(state == expectedState);
    CHECK(prefState == expectedPrefState);
    // Reading a group this build cannot understand must not cost the user the
    // document a later build can: loading never rewrites the file.
    CHECK(ao::test::readFile(libraryPath / "gtk_layout.yaml") == stored);
  }

  TEST_CASE("GtkLayoutStateStore - failed serialization leaves both durable groups unchanged",
            "[gtk][unit][app][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const expectedState = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{10},
       {uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 123, .weight = -1.0, .visible = true}}},
    };
    auto const expectedPrefState = uimodel::ListPresentations::Snapshot{{ListId{10}, "albums"}};
    auto store = GtkLayoutStateStore{libraryPath};
    store.save(expectedState, expectedPrefState);
    auto const before = ao::test::readFile(libraryPath / "gtk_layout.yaml");
    REQUIRE_FALSE(before.empty());

    auto candidateState = expectedState;
    auto candidatePrefState = expectedPrefState;
    // Artist is a flexible field, so a fixed width violates its sizing policy
    // and rejects the column group; its valid sibling must not commit alone.
    candidateState.at(ListId{10}).at(0).field = rt::TrackField::Artist;
    candidateState.at(ListId{10}).at(0).width = 456;
    candidatePrefState.at(ListId{10}) = "artists";
    store.save(candidateState, candidatePrefState);

    CHECK(ao::test::readFile(libraryPath / "gtk_layout.yaml") == before);

    auto const freshStore = GtkLayoutStateStore{libraryPath};
    auto loadedState = uimodel::TrackColumnLayouts::Snapshot{};
    auto loadedPrefState = uimodel::ListPresentations::Snapshot{};
    freshStore.load(loadedState, loadedPrefState);
    CHECK(loadedState == expectedState);
    CHECK(loadedPrefState == expectedPrefState);
  }
} // namespace ao::gtk::test
