// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TuiLayoutStateStore.h"

#include "test/unit/TestFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

#ifdef __APPLE__
#include <unistd.h>
#endif

namespace ao::tui::test
{
  TEST_CASE("TuiLayoutStateStore - missing file preserves seeded state", "[tui][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{7}, {uimodel::TrackColumnState{.field = rt::TrackField::Title, .weight = 2.0}}},
    };
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{7}, "songs"}};
    auto const store = TuiLayoutStateStore{libraryPath};

    store.load(columns, presentations);

    REQUIRE(columns.size() == 1);
    CHECK(columns.contains(ListId{7}));
    REQUIRE(presentations.size() == 1);
    CHECK(presentations.at(ListId{7}) == "songs");
  }

  TEST_CASE("TuiLayoutStateStore - rejects aliases between TUI ConfigStore writers", "[tui][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const workspacePath = libraryPath / ".aobus" / "tui-workspace.yaml";
    auto const layoutPath = tuiLayoutStatePath(libraryPath);
    auto const appConfigPath = libraryPath / "config" / "tui.yaml";

    CHECK(validateTuiConfigStorePaths(libraryPath, workspacePath, appConfigPath));

#if defined(_WIN32) || defined(__APPLE__)
    auto const caseVariantCollisionRes =
      validateTuiConfigStorePaths(libraryPath, layoutPath.parent_path() / "TUI_LAYOUT.YAML", appConfigPath);

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

    auto const layoutCollisionRes = validateTuiConfigStorePaths(libraryPath, layoutPath, appConfigPath);
    REQUIRE_FALSE(layoutCollisionRes);
    CHECK(layoutCollisionRes.error().code == Error::Code::InvalidInput);

    std::filesystem::create_directories(layoutPath.parent_path());
    std::ofstream{layoutPath, std::ios::binary} << "layout-sentinel\n";
    auto const layoutAliasPath = libraryPath / "layout-alias.yaml";
    std::filesystem::create_hard_link(layoutPath, layoutAliasPath);
    auto const physicalCollisionRes = validateTuiConfigStorePaths(libraryPath, layoutAliasPath, appConfigPath);
    REQUIRE_FALSE(physicalCollisionRes);
    CHECK(physicalCollisionRes.error().code == Error::Code::InvalidInput);

    auto const appCollisionRes = validateTuiConfigStorePaths(libraryPath, appConfigPath, appConfigPath);
    REQUIRE_FALSE(appCollisionRes);
    CHECK(appCollisionRes.error().code == Error::Code::InvalidInput);

    auto const fixedPathCollisionRes = validateTuiConfigStorePaths(libraryPath, workspacePath, layoutPath);
    REQUIRE_FALSE(fixedPathCollisionRes);
    CHECK(fixedPathCollisionRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("TuiLayoutStateStore - owns the per-library terminal layout document", "[tui][unit][config][track-columns]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};

    CHECK(tuiLayoutStatePath(libraryPath) == libraryPath / ".aobus" / "tui_layout.yaml");

    auto store = TuiLayoutStateStore{libraryPath};
    auto const runtimeSessionPath = libraryPath / ".aobus" / "tui-workspace.yaml";
    std::ofstream{runtimeSessionPath, std::ios::binary} << "workspace-sentinel\n";
    auto columnLayouts = uimodel::TrackColumnLayouts::Snapshot{};
    columnLayouts[ListId{10}] = {
      uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = -1, .weight = 1.75},
      uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 17, .visible = true},
    };
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{10}, "albums"}};

    REQUIRE(store.save(columnLayouts, presentations));
    CHECK(ao::test::readFile(runtimeSessionPath) == "workspace-sentinel\n");

    auto const serialized = ao::test::readFile(tuiLayoutStatePath(libraryPath));
    CHECK(serialized == "trackView.columnLayouts:\n"
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
    auto const reopenedStore = TuiLayoutStateStore{libraryPath};
    reopenedStore.load(loadedColumns, loadedPresentations);

    CHECK(loadedColumns == columnLayouts);
    CHECK(loadedPresentations == presentations);
  }

  TEST_CASE("TuiLayoutStateStore - loads valid groups independently and never rewrites on load",
            "[tui][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const configPath = tuiLayoutStatePath(libraryPath);
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
    auto const store = TuiLayoutStateStore{libraryPath};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{7}, {uimodel::TrackColumnState{.field = rt::TrackField::Title, .weight = 2.0}}},
    };
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{7}, "songs"}};

    store.load(columns, presentations);

    REQUIRE(columns.size() == 1);
    CHECK(columns.contains(ListId{7}));
    REQUIRE(presentations.size() == 1);
    CHECK(presentations.at(ListId{42}) == "albums");
    CHECK(ao::test::readFile(configPath) == stored);
  }

  TEST_CASE("TuiLayoutStateStore - valid columns load when presentation preferences are rejected",
            "[tui][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const configPath = tuiLayoutStatePath(libraryPath);
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
    auto const store = TuiLayoutStateStore{libraryPath};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{};
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{7}, "songs"}};

    store.load(columns, presentations);

    REQUIRE(columns.size() == 1);
    REQUIRE(columns.contains(ListId{42}));
    REQUIRE(columns.at(ListId{42}).size() == 1);
    CHECK(columns.at(ListId{42})[0].width == 17);
    REQUIRE(presentations.size() == 1);
    CHECK(presentations.at(ListId{7}) == "songs");
    CHECK(ao::test::readFile(configPath) == stored);
  }

  TEST_CASE("TuiLayoutStateStore - serialization failure preserves state and permits a later retry",
            "[tui][regression][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const libraryPath = std::filesystem::path{tempDir.path()};
    auto const configPath = tuiLayoutStatePath(libraryPath);
    std::filesystem::create_directories(configPath.parent_path());
    std::ofstream{configPath, std::ios::binary} << "future.owner:\n  value: keep-me\n";
    auto store = TuiLayoutStateStore{libraryPath};
    auto columns = uimodel::TrackColumnLayouts::Snapshot{
      {ListId{10}, {uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 17}}},
    };
    auto presentations = uimodel::ListPresentations::Snapshot{{ListId{10}, "albums"}};

    REQUIRE(store.save(columns, presentations));
    auto const before = ao::test::readFile(configPath);
    CHECK(before.contains("future.owner:\n  value: keep-me\n"));
    CHECK(before.contains("width: 17"));
    CHECK(before.contains("presentationId: \"albums\""));

    columns.at(ListId{10})[0] = uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = 29, .weight = -1.0};
    presentations.at(ListId{10}) = "artists";
    auto const failedSaveRes = store.save(columns, presentations);

    REQUIRE_FALSE(failedSaveRes);
    CHECK(ao::test::readFile(configPath) == before);

    columns.at(ListId{10})[0] = uimodel::TrackColumnState{.field = rt::TrackField::Artist, .width = -1, .weight = 2.0};
    REQUIRE(store.save(columns, presentations));

    auto const after = ao::test::readFile(configPath);
    CHECK(after.contains("future.owner:\n  value: keep-me\n"));
    CHECK(after.contains("field: \"artist\""));
    CHECK(after.contains("weight: 2"));
    CHECK(after.contains("presentationId: \"artists\""));
  }
} // namespace ao::tui::test
