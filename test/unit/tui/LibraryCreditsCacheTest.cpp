// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "tui/LibraryController.h"
#include <ao/async/LoopExecutor.h>
#include <ao/library/Credits.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("LibraryController credits - redraw borrows the same owning ordered list until focus changes",
            "[tui][unit][library]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(tempDir, std::make_unique<async::LoopExecutor>());
    auto const expected = std::vector<library::Credit>{{"A", library::CreditKind::Performer, "Piano"},
                                                       {"A", library::CreditKind::Performer, "Voice"},
                                                       {"A", library::CreditKind::Performer, "Piano"}};
    auto const credited = rt::test::addRuntimeTrack(*runtimePtr, {.title = "A", .credits = expected});
    auto const empty = rt::test::addRuntimeTrack(*runtimePtr, {.title = "B"});
    auto catalog = uimodel::TrackPresentationCatalog{runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, runtimePtr->library().changes()};
    auto controller = LibraryController{runtimePtr->library(),
                                        runtimePtr->views(),
                                        runtimePtr->workspace(),
                                        ao::test::englishMessageCatalog(),
                                        presentations};
    std::ignore = controller.setPresentation("songs");
    controller.setSelectedTrackIndex(0);
    REQUIRE(controller.selectedTrackView().track->id == credited);
    auto const& first = controller.focusedCredits()[3];
    REQUIRE(first.optValue);
    CHECK(*first.optValue == expected);
    auto const* storage = first.optValue->data();
    // Equal entry storage proves that redraw did not replace the owned list;
    // snapshot-open counts are not exposed by the production contract.
    controller.toggleFocusedMark();
    controller.setSelectedTrackIndex(0);
    auto const& redrawn = controller.focusedCredits()[3];
    CHECK(&redrawn == &first);
    REQUIRE(redrawn.optValue);
    CHECK(redrawn.optValue->data() == storage);
    CHECK(*redrawn.optValue == expected);
    controller.setSelectedTrackIndex(1);
    REQUIRE(controller.selectedTrackView().track->id == empty);

    for (auto const& section : controller.focusedCredits())
    {
      REQUIRE(section.optValue);
      CHECK(section.optValue->empty());
      CHECK_FALSE(section.mixed);
    }

    controller.setSelectedTrackIndex(0);
    CHECK(controller.focusedCredits()[3].optValue == expected);
    std::ignore = controller.reloadActiveList();
    CHECK(controller.focusedCredits()[3].optValue == expected);
  }

  TEST_CASE("LibraryController credits - publication replaces clears and deletes the cached focused facts",
            "[tui][unit][library]")
  {
    using library::CreditKind;
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(tempDir, std::make_unique<async::LoopExecutor>());
    auto const id =
      rt::test::addRuntimeTrack(*runtimePtr, {.title = "A", .credits = {{"Before", CreditKind::Conductor, "Piano"}}});
    auto catalog = uimodel::TrackPresentationCatalog{runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, runtimePtr->library().changes()};
    auto controller = LibraryController{runtimePtr->library(),
                                        runtimePtr->views(),
                                        runtimePtr->workspace(),
                                        ao::test::englishMessageCatalog(),
                                        presentations};
    CHECK(controller.focusedCredits()[0].optValue ==
          std::vector<library::Credit>{{"Before", CreditKind::Conductor, "Piano"}});
    rt::test::updateRuntimeTrack(
      *runtimePtr, id, [](auto& spec) { spec.credits = {{"After", CreditKind::Soloist, "Voice"}}; });
    CHECK(controller.focusedCredits()[0].optValue->empty());
    CHECK(controller.focusedCredits()[2].optValue ==
          std::vector<library::Credit>{{"After", CreditKind::Soloist, "Voice"}});
    rt::test::updateRuntimeTrack(*runtimePtr, id, [](auto& spec) { spec.credits.clear(); });
    auto const& cleared = controller.focusedCredits()[2];
    REQUIRE(cleared.optValue);
    CHECK(cleared.optValue->empty());
    auto const deletedRes =
      rt::test::runRuntimeTask(*runtimePtr, runtimePtr->library().commands().deleteTrackAsync(id));
    REQUIRE(deletedRes);
    CHECK(controller.tracks().empty());

    for (auto const& section : controller.focusedCredits())
    {
      CHECK_FALSE(section.optValue);
      CHECK_FALSE(section.mixed);
    }

    auto const newId =
      rt::test::addRuntimeTrack(*runtimePtr, {.title = "New", .credits = {{"New", CreditKind::Performer, "Violin"}}});
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == newId);
    CHECK(controller.focusedCredits()[3].optValue ==
          std::vector<library::Credit>{{"New", CreditKind::Performer, "Violin"}});
  }

  TEST_CASE("LibraryController credits - library reset replaces cached credits from a disposable restore",
            "[tui][unit][library]")
  {
    using library::CreditKind;
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(tempDir, std::make_unique<async::LoopExecutor>());
    std::ignore = rt::test::addRuntimeTrack(
      *runtimePtr, {.title = "Before", .credits = {{"Before", CreditKind::Performer, "Piano"}}});
    auto catalog = uimodel::TrackPresentationCatalog{runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, runtimePtr->library().changes()};
    auto controller = LibraryController{runtimePtr->library(),
                                        runtimePtr->views(),
                                        runtimePtr->workspace(),
                                        ao::test::englishMessageCatalog(),
                                        presentations};
    CHECK(controller.focusedCredits()[3].optValue ==
          std::vector<library::Credit>{{"Before", CreditKind::Performer, "Piano"}});
    auto const path = tempDir.path() / "restore.yaml";
    {
      auto yaml = std::ofstream{path};
      yaml << "version: 7\nexport_mode: full\nlibrary:\n  resources: []\n  tracks:\n"
              "    - uri: restored.flac\n      title: After\n      credits:\n"
              "        - name: After\n          kind: performer\n          role: Voice\n  lists: []\n";
      REQUIRE(yaml.good());
    }
    auto planRes = rt::test::runRuntimeTask(
      *runtimePtr, runtimePtr->library().jobs().prepareLibraryImportAsync(path, rt::ImportMode::Restore));
    REQUIRE(planRes);
    auto appliedRes = rt::test::runRuntimeTask(
      *runtimePtr, runtimePtr->library().jobs().applyLibraryImportPlanAsync(std::move(*planRes)));
    REQUIRE(appliedRes);
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.focusedCredits()[3].optValue ==
          std::vector<library::Credit>{{"After", CreditKind::Performer, "Voice"}});
  }
} // namespace ao::tui::test
