// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/LayoutStateStore.h"

#include "test/unit/TestFixtureSupport.h"
#include "tui/PanelWidths.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>

#ifdef __APPLE__
#include <unistd.h>

#include <sys/unistd.h>
#endif

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

namespace ao::tui::test
{
  TEST_CASE("LayoutStateStore - missing file preserves seeded state", "[tui][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{7}, {uimodel::TrackColumnState{.field = rt::TrackField::Title, .weight = 2.0}}},
    };
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{7}, "songs"}};
    auto const store = LayoutStateStore{libraryPath};

    auto widths = PanelWidths{};
    bool navigationEnabled = true;
    store.load(columns, presentations, navigationEnabled, widths);

    REQUIRE(columns.size() == 1);
    CHECK(columns.contains(ListId{7}));
    REQUIRE(presentations.size() == 1);
    CHECK(presentations.at(ListId{7}) == "songs");
  }

  TEST_CASE("LayoutStateStore - rejects aliases between TUI ConfigStore writers", "[tui][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const workspacePath = libraryPath / ".aobus" / "tui-workspace.yaml";
    auto const layoutPath = layoutStatePath(libraryPath);
    auto const appConfigPath = libraryPath / "config" / "tui.yaml";

    CHECK(validateConfigStorePaths(libraryPath, workspacePath, appConfigPath));

#if defined(_WIN32) || defined(__APPLE__)
    auto const caseVariantCollisionRes =
      validateConfigStorePaths(libraryPath, layoutPath.parent_path() / "TUI_LAYOUT.YAML", appConfigPath);

#ifdef __APPLE__

    if (::pathconf(libraryPath.c_str(), _PC_CASE_SENSITIVE) == 1)
    {
      CHECK(caseVariantCollisionRes);
    }
    else
#endif
    {
      REQUIRE_FALSE(caseVariantCollisionRes);
      CHECK(caseVariantCollisionRes.error().code == Error::Code::InvalidInput);
    }

#endif

    auto const layoutCollisionRes = validateConfigStorePaths(libraryPath, layoutPath, appConfigPath);
    REQUIRE_FALSE(layoutCollisionRes);
    CHECK(layoutCollisionRes.error().code == Error::Code::InvalidInput);

    std::filesystem::create_directories(layoutPath.parent_path());
    std::ofstream{layoutPath, std::ios::binary} << "layout-sentinel\n";
    auto const layoutAliasPath = libraryPath / "layout-alias.yaml";
    std::filesystem::create_hard_link(layoutPath, layoutAliasPath);
    auto const physicalCollisionRes = validateConfigStorePaths(libraryPath, layoutAliasPath, appConfigPath);
    REQUIRE_FALSE(physicalCollisionRes);
    CHECK(physicalCollisionRes.error().code == Error::Code::InvalidInput);

    auto const appCollisionRes = validateConfigStorePaths(libraryPath, appConfigPath, appConfigPath);
    REQUIRE_FALSE(appCollisionRes);
    CHECK(appCollisionRes.error().code == Error::Code::InvalidInput);

    auto const fixedPathCollisionRes = validateConfigStorePaths(libraryPath, workspacePath, layoutPath);
    REQUIRE_FALSE(fixedPathCollisionRes);
    CHECK(fixedPathCollisionRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("LayoutStateStore - owns the per-library terminal layout document", "[tui][unit][config][track-columns]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};

    CHECK(layoutStatePath(libraryPath) == libraryPath / ".aobus" / "tui_layout.yaml");

    auto store = LayoutStateStore{libraryPath};
    auto const runtimeSessionPath = libraryPath / ".aobus" / "tui-workspace.yaml";
    std::ofstream{runtimeSessionPath, std::ios::binary} << "workspace-sentinel\n";
    auto columnLayouts = uimodel::TrackColumnLayouts::Snapshot{};
    columnLayouts[ListId{10}] = {
      uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = -1, .weight = 1.75},
      uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 17, .visible = true},
    };
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{10}, "albums"}};

    REQUIRE(store.save(columnLayouts, presentations, true, PanelWidths{}));
    CHECK(ao::test::readFile(runtimeSessionPath) == "workspace-sentinel\n");

    auto const serialized = ao::test::readFile(layoutStatePath(libraryPath));
    CHECK(serialized == "panels:\n"
                        "  version: 1\n"
                        "  navigation: 0\n"
                        "  detail: 0\n"
                        "navigation:\n"
                        "  version: 1\n"
                        "  enabled: true\n"
                        "trackView.columnLayouts:\n"
                        "  version: 2\n"
                        "  layouts:\n"
                        "    - listId: 10\n"
                        "      columns:\n"
                        "        - field: \"artist\"\n"
                        "          width: -1\n"
                        "          weight: 1.75\n"
                        "          visible: true\n"
                        "        - field: \"duration\"\n"
                        "          width: 17\n"
                        "          weight: -1\n"
                        "          visible: true\n"
                        "trackView.presentations:\n"
                        "  version: 1\n"
                        "  preferences:\n"
                        "    - listId: 10\n"
                        "      presentationId: \"albums\"\n");

    auto loadedColumns = uimodel::TrackColumnLayouts::Snapshot{};
    auto loadedPresentations = uimodel::ListPresentations::Snapshot{};
    auto const reopenedStore = LayoutStateStore{libraryPath};
    auto widths = PanelWidths{};
    bool navigationEnabled = true;
    reopenedStore.load(loadedColumns, loadedPresentations, navigationEnabled, widths);

    CHECK(loadedColumns == columnLayouts);
    CHECK(loadedPresentations == presentations);
  }

  TEST_CASE("LayoutStateStore - loads valid groups independently and never rewrites on load",
            "[tui][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const configPath = layoutStatePath(libraryPath);
    std::filesystem::create_directories(configPath.parent_path());
    auto const stored = std::string{"future.owner:\n"
                                    "  value: keep-me\n"
                                    "trackView.columnLayouts:\n"
                                    "  version: 99\n"
                                    "  layouts: []\n"
                                    "trackView.presentations:\n"
                                    "  version: 1\n"
                                    "  preferences:\n"
                                    "    - listId: 42\n"
                                    "      presentationId: albums\n"};
    std::ofstream{configPath, std::ios::binary} << stored;
    auto const store = LayoutStateStore{libraryPath};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{7}, {uimodel::TrackColumnState{.field = rt::TrackField::Title, .weight = 2.0}}},
    };
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{7}, "songs"}};

    auto widths = PanelWidths{};
    bool navigationEnabled = true;
    store.load(columns, presentations, navigationEnabled, widths);

    REQUIRE(columns.size() == 1);
    CHECK(columns.contains(ListId{7}));
    REQUIRE(presentations.size() == 1);
    CHECK(presentations.at(ListId{42}) == "albums");
    CHECK(ao::test::readFile(configPath) == stored);
  }

  TEST_CASE("LayoutStateStore - valid columns load when presentation preferences are rejected",
            "[tui][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const configPath = layoutStatePath(libraryPath);
    std::filesystem::create_directories(configPath.parent_path());
    auto const stored = std::string{"trackView.columnLayouts:\n"
                                    "  version: 2\n"
                                    "  layouts:\n"
                                    "    - listId: 42\n"
                                    "      columns:\n"
                                    "        - field: duration\n"
                                    "          width: 17\n"
                                    "          weight: -1\n"
                                    "          visible: true\n"
                                    "trackView.presentations:\n"
                                    "  version: 99\n"
                                    "  preferences: []\n"};
    std::ofstream{configPath, std::ios::binary} << stored;
    auto const store = LayoutStateStore{libraryPath};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{};
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{7}, "songs"}};

    auto widths = PanelWidths{};
    bool navigationEnabled = true;
    store.load(columns, presentations, navigationEnabled, widths);

    REQUIRE(columns.size() == 1);
    REQUIRE(columns.contains(ListId{42}));
    REQUIRE(columns.at(ListId{42}).size() == 1);
    CHECK(columns.at(ListId{42})[0].width == 17);
    REQUIRE(presentations.size() == 1);
    CHECK(presentations.at(ListId{7}) == "songs");
    CHECK(ao::test::readFile(configPath) == stored);
  }

  TEST_CASE("LayoutStateStore - serialization failure preserves state and permits a later retry",
            "[tui][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const configPath = layoutStatePath(libraryPath);
    std::filesystem::create_directories(configPath.parent_path());
    std::ofstream{configPath, std::ios::binary} << "future.owner:\n  value: keep-me\n";
    auto store = LayoutStateStore{libraryPath};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{10}, {uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 17}}},
    };
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{10}, "albums"}};

    REQUIRE(store.save(columns, presentations, true, PanelWidths{}));
    auto const before = ao::test::readFile(configPath);
    CHECK(before.contains("future.owner:\n  value: keep-me\n"));
    CHECK(before.contains("width: 17"));
    CHECK(before.contains("presentationId: \"albums\""));

    columns.at(ListId{10})[0] = uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 29, .weight = -1.0};
    presentations.at(ListId{10}) = "artists";
    auto const failedSaveRes = store.save(columns, presentations, true, PanelWidths{});

    REQUIRE_FALSE(failedSaveRes);
    CHECK(ao::test::readFile(configPath) == before);

    columns.at(ListId{10})[0] = uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = -1, .weight = 2.0};
    REQUIRE(store.save(columns, presentations, true, PanelWidths{}));

    auto const after = ao::test::readFile(configPath);
    CHECK(after.contains("future.owner:\n  value: keep-me\n"));
    CHECK(after.contains("field: \"artist\""));
    CHECK(after.contains("weight: 2"));
    CHECK(after.contains("presentationId: \"artists\""));
  }

  TEST_CASE("LayoutStateStore - navigation visibility round trips independently of focus and geometry",
            "[tui][unit][config]")
  {
    auto const directory = ao::test::TempDir{};
    auto store = LayoutStateStore{directory.path()};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{};
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{7}, "songs"}};
    REQUIRE(store.save(columns, presentations, false, PanelWidths{}));
    auto reopened = LayoutStateStore{directory.path()};
    auto widths = PanelWidths{};
    bool enabled = true;
    presentations.clear();
    reopened.load(columns, presentations, enabled, widths);
    CHECK_FALSE(enabled);
    CHECK(presentations.at(ListId{7}) == "songs");
    REQUIRE(reopened.save(columns, presentations, true, widths));
    auto again = LayoutStateStore{directory.path()};
    enabled = false;
    again.load(columns, presentations, enabled, widths);
    CHECK(enabled);
  }

  TEST_CASE("LayoutStateStore - malformed navigation defaults without discarding other groups",
            "[tui][regression][config]")
  {
    for (auto const* group : {"navigation: {version: 2, enabled: false}",
                              "navigation: {version: 1, enabled: nope}",
                              "navigation: []",
                              "navigation: {enabled: false}"})
    {
      auto const directory = ao::test::TempDir{};
      auto const path = layoutStatePath(directory.path());
      std::filesystem::create_directories(path.parent_path());
      {
        auto output = std::ofstream{path};
        output << group << "\n";
      }
      auto store = LayoutStateStore{directory.path()};
      auto columns = uimodel::TrackColumnLayouts::Snapshot{};
      auto presentations = uimodel::ListPresentations::Snapshot{{ListId{7}, "songs"}};
      auto widths = PanelWidths{};
      bool enabled = false;
      store.load(columns, presentations, enabled, widths);
      CHECK(enabled);
      CHECK(presentations.at(ListId{7}) == "songs");
      REQUIRE(store.save(columns, presentations, false, widths));
      auto reopened = LayoutStateStore{directory.path()};
      reopened.load(columns, presentations, enabled, widths);
      CHECK_FALSE(enabled);
    }
  }

  TEST_CASE("LayoutStateStore - panel widths round trip and missing or malformed widths use automatic sizing",
            "[tui][unit][config][panel-resize]")
  {
    auto const directory = ao::test::TempDir{};
    auto store = LayoutStateStore{directory.path()};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{};
    auto presentations = uimodel::ListPresentations::Snapshot{};
    auto widths = PanelWidths{.navigation = 32, .detail = 48};
    REQUIRE(store.save(columns, presentations, true, widths));
    auto reopened = LayoutStateStore{directory.path()};
    auto loaded = PanelWidths{};
    bool enabled = false;
    reopened.load(columns, presentations, enabled, loaded);
    CHECK(loaded == widths);

    for (auto const* document : {"navigation: {version: 1, enabled: false}\n",
                                 "panels: {version: 2, navigation: 32, detail: 48}\n",
                                 "panels: {version: 1, navigation: -1, detail: 48}\n",
                                 "panels: {version: 1, navigation: 32, detail: broken}\n"})
    {
      std::ofstream{layoutStatePath(directory.path())} << document;
      auto invalid = LayoutStateStore{directory.path()};
      loaded = widths;
      invalid.load(columns, presentations, enabled, loaded);
      CHECK(loaded == PanelWidths{});
    }
  }
} // namespace ao::tui::test
