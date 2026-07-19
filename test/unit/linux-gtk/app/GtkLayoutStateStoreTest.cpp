// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkLayoutStateStore.h"

#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace ao::gtk::test
{
  TEST_CASE("GtkLayoutStateStore - persists column layout preferences", "[gtk][unit][app][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};

    SECTION("Load non-existent config returns default")
    {
      auto const store = GtkLayoutStateStore{libraryPath};
      auto newState = uimodel::TrackColumnLayoutState{};
      auto newPrefState = uimodel::ListPresentationPreferenceState{};
      store.load(newState, newPrefState);
      // Not found should not modify
      CHECK(newState.listLayouts.empty());
      CHECK(newPrefState.presentations.empty());
    }

    SECTION("Save and load layout state")
    {
      {
        auto store = GtkLayoutStateStore{libraryPath};
        auto state = uimodel::TrackColumnLayoutState{};
        auto prefState = uimodel::ListPresentationPreferenceState{};
        state.listLayouts[ListId{10}] = {
          uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = -1, .weight = 1.75}};
        state.listLayouts[ListId{20}] = {
          uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 200, .weight = -1.0}};
        prefState.presentations[ListId{10}] = "albums";
        store.save(state, prefState);

        auto const serialized = ao::test::readFile(libraryPath / "gtk_layout.yaml");
        CHECK(serialized == "trackView.columnLayouts:\n"
                            "  version: 1\n"
                            "  layouts:\n"
                            "    - listId: 10\n"
                            "      columns:\n"
                            "        - field: \"artist\"\n"
                            "          width: -1\n"
                            "          weight: 1.75\n"
                            "    - listId: 20\n"
                            "      columns:\n"
                            "        - field: \"duration\"\n"
                            "          width: 200\n"
                            "          weight: -1\n"
                            "trackView.presentations:\n"
                            "  version: 1\n"
                            "  preferences:\n"
                            "    - listId: 10\n"
                            "      presentationId: \"albums\"\n");
      }

      {
        auto const store = GtkLayoutStateStore{libraryPath};
        auto state = uimodel::TrackColumnLayoutState{};
        auto prefState = uimodel::ListPresentationPreferenceState{};
        store.load(state, prefState);
        REQUIRE(state.listLayouts.size() == 2);
        CHECK(state.listLayouts[ListId{10}][0].field == rt::TrackField::Artist);
        CHECK(state.listLayouts[ListId{10}][0].weight == 1.75);
        CHECK(state.listLayouts[ListId{20}][0].field == rt::TrackField::Duration);
        CHECK(state.listLayouts[ListId{20}][0].width == 200);
        REQUIRE(prefState.presentations.size() == 1);
        CHECK(prefState.presentations[ListId{10}] == "albums");
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
      auto state = uimodel::TrackColumnLayoutState{};
      state.listLayouts[ListId{7}] = {
        uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 123},
      };
      auto prefState = uimodel::ListPresentationPreferenceState{};
      store.load(state, prefState);

      REQUIRE(state.listLayouts.size() == 1);
      REQUIRE(state.listLayouts.contains(ListId{7}));
      REQUIRE(state.listLayouts[ListId{7}].size() == 1);
      CHECK(state.listLayouts[ListId{7}][0].field == rt::TrackField::Artist);
      CHECK(state.listLayouts[ListId{7}][0].width == 123);
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
      auto state = uimodel::TrackColumnLayoutState{};
      auto prefState = uimodel::ListPresentationPreferenceState{};
      prefState.presentations[ListId{7}] = "artists";
      store.load(state, prefState);

      REQUIRE(prefState.presentations.size() == 1);
      CHECK(prefState.presentations.at(ListId{7}) == "artists");
    }

    SECTION("Load a valid sibling group when the column layout group is unsupported")
    {
      std::filesystem::create_directories(libraryPath);
      auto output = std::ofstream{libraryPath / "gtk_layout.yaml"};
      output << "trackView.columnLayouts:\n"
                "  version: 2\n"
                "  layouts: []\n"
                "trackView.presentations:\n"
                "  version: 1\n"
                "  preferences:\n"
                "    - listId: 42\n"
                "      presentationId: albums\n";
      output.close();

      auto const store = GtkLayoutStateStore{libraryPath};
      auto state = uimodel::TrackColumnLayoutState{};
      state.listLayouts[ListId{7}] = {
        uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 123},
      };
      auto prefState = uimodel::ListPresentationPreferenceState{};
      prefState.presentations[ListId{7}] = "artists";
      store.load(state, prefState);

      REQUIRE(state.listLayouts.size() == 1);
      CHECK(state.listLayouts.contains(ListId{7}));
      REQUIRE(prefState.presentations.size() == 1);
      CHECK(prefState.presentations.at(ListId{42}) == "albums");
    }

    SECTION("Serialization failure leaves both durable groups unchanged")
    {
      auto store = GtkLayoutStateStore{libraryPath};
      auto state = uimodel::TrackColumnLayoutState{};
      state.listLayouts[ListId{10}] = {
        uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 123},
      };
      auto prefState = uimodel::ListPresentationPreferenceState{};
      prefState.presentations[ListId{10}] = "albums";
      store.save(state, prefState);
      auto const before = ao::test::readFile(libraryPath / "gtk_layout.yaml");

      state.listLayouts[ListId{10}][0].width = 456;
      prefState.presentations[kInvalidListId] = "invalid";
      store.save(state, prefState);

      CHECK(ao::test::readFile(libraryPath / "gtk_layout.yaml") == before);
    }
  }
} // namespace ao::gtk::test
