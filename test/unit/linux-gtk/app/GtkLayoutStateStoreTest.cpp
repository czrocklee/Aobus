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
  TEST_CASE("GtkLayoutStateStore - persists column layout preferences", "[gtk][unit][app][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};

    SECTION("Load non-existent config returns default")
    {
      auto const store = GtkLayoutStateStore{libraryPath};
      auto newState = uimodel::TrackColumnLayouts::Snapshot{};
      auto newPrefState = uimodel::ListPresentations::Snapshot{};
      store.load(newState, newPrefState);
      // Not found should not modify
      CHECK(newState.empty());
      CHECK(newPrefState.empty());
    }

    SECTION("Save and load layout state")
    {
      {
        auto store = GtkLayoutStateStore{libraryPath};
        auto state = uimodel::TrackColumnLayouts::Snapshot{};
        auto prefState = uimodel::ListPresentations::Snapshot{};
        state[ListId{10}] = {uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = -1, .weight = 1.75}};
        state[ListId{20}] = {
          uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 200, .weight = -1.0}};
        prefState[ListId{10}] = "albums";
        store.save(state, prefState);

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
      }

      {
        auto const store = GtkLayoutStateStore{libraryPath};
        auto state = uimodel::TrackColumnLayouts::Snapshot{};
        auto prefState = uimodel::ListPresentations::Snapshot{};
        store.load(state, prefState);
        REQUIRE(state.size() == 2);
        CHECK(state[ListId{10}][0].field == rt::TrackField::Artist);
        CHECK(state[ListId{10}][0].weight == 1.75);
        CHECK(state[ListId{20}][0].field == rt::TrackField::Duration);
        CHECK(state[ListId{20}][0].width == 200);
        REQUIRE(prefState.size() == 1);
        CHECK(prefState[ListId{10}] == "albums");
      }
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
      auto state = uimodel::TrackColumnLayouts::Snapshot{};
      state[ListId{7}] = {
        uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 123},
      };
      auto prefState = uimodel::ListPresentations::Snapshot{};
      store.load(state, prefState);

      REQUIRE(state.size() == 1);
      REQUIRE(state.contains(ListId{7}));
      REQUIRE(state[ListId{7}].size() == 1);
      CHECK(state[ListId{7}][0].field == rt::TrackField::Artist);
      CHECK(state[ListId{7}][0].width == 123);
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
      auto state = uimodel::TrackColumnLayouts::Snapshot{};
      auto prefState = uimodel::ListPresentations::Snapshot{};
      prefState[ListId{7}] = "artists";
      store.load(state, prefState);

      REQUIRE(prefState.size() == 1);
      CHECK(prefState.at(ListId{7}) == "artists");
    }

    SECTION("Load a valid sibling group when the column layout group is unsupported")
    {
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

      auto const store = GtkLayoutStateStore{libraryPath};
      auto state = uimodel::TrackColumnLayouts::Snapshot{};
      state[ListId{7}] = {
        uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 123},
      };
      auto prefState = uimodel::ListPresentations::Snapshot{};
      prefState[ListId{7}] = "artists";
      store.load(state, prefState);

      REQUIRE(state.size() == 1);
      CHECK(state.contains(ListId{7}));
      REQUIRE(prefState.size() == 1);
      CHECK(prefState.at(ListId{42}) == "albums");
      // Reading a group this build cannot understand must not cost the user the
      // document a later build can: loading never rewrites the file.
      CHECK(ao::test::readFile(libraryPath / "gtk_layout.yaml") == stored);
    }

    SECTION("Serialization failure leaves both durable groups unchanged")
    {
      auto store = GtkLayoutStateStore{libraryPath};
      auto state = uimodel::TrackColumnLayouts::Snapshot{};
      state[ListId{10}] = {
        uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 123},
      };
      auto prefState = uimodel::ListPresentations::Snapshot{};
      prefState[ListId{10}] = "albums";
      store.save(state, prefState);
      auto const before = ao::test::readFile(libraryPath / "gtk_layout.yaml");

      state[ListId{10}][0].width = 456;
      prefState[kInvalidListId] = "invalid";
      store.save(state, prefState);

      CHECK(ao::test::readFile(libraryPath / "gtk_layout.yaml") == before);
    }
  }
} // namespace ao::gtk::test
