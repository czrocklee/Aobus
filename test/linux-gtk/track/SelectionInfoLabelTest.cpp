// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/SelectionInfoLabel.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryJobs.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

#include <chrono>
#include <filesystem>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("SelectionInfoLabel - binds selection changes to summary text")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const reply = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    auto label = SelectionInfoLabel{
      runtime.views(), runtime.workspace(), runtime.library().changes(), ao::test::englishMessageCatalog()};
    auto const& text = dynamic_cast<Gtk::Label const&>(label.widget());

    CHECK(text.has_css_class("dim-label"));

    auto const emptyText = text.get_text();

    REQUIRE(runtime.views().setSelection(reply, {TrackId{1}, TrackId{2}}));
    auto const selectedText = text.get_text();
    CHECK(selectedText != emptyText);

    REQUIRE(runtime.views().setSelection(reply, {}));
    CHECK(text.get_text() == emptyText);
  }

  TEST_CASE("SelectionInfoLabel - follows only the active runtime view")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto firstTrackId = kInvalidTrackId;
    auto secondTrackId = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        {
                          firstTrackId = library::test::addTrackWithUniqueFixtureUri(
                            musicLibrary, {.title = "First selection", .duration = std::chrono::minutes{2}});
                          secondTrackId = library::test::addTrackWithUniqueFixtureUri(
                            musicLibrary, {.title = "Second selection", .duration = std::chrono::minutes{3}});
                        }};
    auto& runtime = fixture.runtime();
    REQUIRE(firstTrackId != kInvalidTrackId);
    REQUIRE(secondTrackId != kInvalidTrackId);
    auto const listId = ao::test::requireValue(
      runGtkTask(runtime, fixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Second view"})));
    auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
    REQUIRE(firstViewId != secondViewId);

    auto label = SelectionInfoLabel{
      runtime.views(), runtime.workspace(), runtime.library().changes(), ao::test::englishMessageCatalog()};
    auto const& text = dynamic_cast<Gtk::Label const&>(label.widget());
    CHECK(text.get_text().empty());

    REQUIRE(runtime.views().setSelection(secondViewId, {secondTrackId}));
    CHECK(text.get_text() == "1 item selected (3:00)");

    {
      auto initialLabel = SelectionInfoLabel{
        runtime.views(), runtime.workspace(), runtime.library().changes(), ao::test::englishMessageCatalog()};
      auto const& initialText = dynamic_cast<Gtk::Label const&>(initialLabel.widget());
      CHECK(initialText.get_text() == "1 item selected (3:00)");
    }

    REQUIRE(runtime.workspace().focusView(firstViewId));
    drainGtkEvents();
    CHECK(text.get_text().empty());

    REQUIRE(runtime.views().setSelection(firstViewId, {firstTrackId}));
    CHECK(text.get_text() == "1 item selected (2:00)");

    // A background view may change without replacing the active view's summary.
    REQUIRE(runtime.views().setSelection(secondViewId, {firstTrackId, secondTrackId}));
    CHECK(text.get_text() == "1 item selected (2:00)");

    REQUIRE(runtime.workspace().focusView(secondViewId));
    drainGtkEvents();
    CHECK(text.get_text() == "2 items selected (5:00)");
  }

  TEST_CASE("SelectionInfoLabel - committed rescan and deletion refresh duration without a selection change")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto selected = kInvalidTrackId;
    auto other = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& storage)
      {
        selected = library::test::addTrack(storage, {.uri = "selected.flac", .duration = std::chrono::minutes{2}});
        other = library::test::addTrack(storage, {.uri = "other.flac", .duration = std::chrono::minutes{3}});
      }};
    auto& runtime = fixture.runtime();
    auto const view = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    REQUIRE(runtime.views().setSelection(view, {selected}));
    drainGtkEvents();
    auto label = SelectionInfoLabel{
      runtime.views(), runtime.workspace(), runtime.library().changes(), ao::test::englishMessageCatalog()};
    auto const& text = dynamic_cast<Gtk::Label const&>(label.widget());
    REQUIRE(text.get_text() == "1 item selected (2:00)");

    bool selectionChanged = false;
    auto const selectionSub = runtime.views().onSelectionChanged([&](auto const&) { selectionChanged = true; });
    auto& commands = fixture.owner().libraryCommands();
    REQUIRE(runGtkTask(runtime, commands.deleteTrackAsync(other)));
    CHECK(text.get_text() == "1 item selected (2:00)");

    std::filesystem::copy_file(
      audio::test::requireAudioFixture("basic_metadata.flac"), runtime.musicRoot() / "selected.flac");
    auto& jobs = fixture.owner().libraryJobs();
    auto plan = ao::test::requireValue(runGtkTask(runtime, jobs.buildScanPlanAsync()));
    auto const result = ao::test::requireValue(runGtkTask(runtime, jobs.applyScanPlanAsync(std::move(plan))));
    REQUIRE(result.mutatedIds == std::vector{selected});
    CHECK(text.get_text() == "1 item selected (0:01)");

    REQUIRE(runGtkTask(runtime, commands.deleteTrackAsync(selected)));
    CHECK_FALSE(selectionChanged);
    CHECK(runtime.workspace().snapshot().activeViewId == view);
    CHECK(runtime.views().trackListState(view).selection == std::vector{selected});
    CHECK(text.get_text() == "1 item selected");
  }
} // namespace ao::gtk::test
