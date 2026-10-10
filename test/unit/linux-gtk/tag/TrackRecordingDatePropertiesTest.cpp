// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/AppRuntime.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <string_view>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    Gtk::Entry* entryWithText(Gtk::Widget& widget, std::string_view text)
    {
      auto const entries = collectAll<Gtk::Entry>(widget);
      auto const it =
        std::ranges::find_if(entries, [text](auto const* entry) { return entry->get_text().raw() == text; });
      REQUIRE(it != entries.end());
      return *it;
    }
  } // namespace

  TEST_CASE("TrackPropertiesDialog - invalid recording date blocks every field until corrected",
            "[gtk][integration][tag][dialog][recording-date]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        {
                          trackId = library::test::addTrackWithUniqueFixtureUri(
                            musicLibrary, {.title = "Date edit", .recordingDate = {.year = 1955, .month = 6}});
                        }};
    auto& runtime = fixture.runtime();
    auto const& catalog = ao::test::englishMessageCatalog();
    auto cache = TrackRowCache{runtime.library(), catalog};
    auto parent = Gtk::Window{};
    auto dialog = TrackPropertiesDialog{
      parent, runtime.async(), runtime.library(), runtime.completion(), catalog, cache, std::vector{trackId}};
    parent.present();
    dialog.present();
    drainGtkEvents();
    auto* const date = entryWithText(dialog, "1955-06");
    auto* const title = entryWithText(dialog, "Date edit");
    auto* const save = findButtonByLabel(dialog, "Save");
    REQUIRE(save != nullptr);

    title->set_text("Changed");
    date->set_text("1955-02-30");
    CHECK(date->has_css_class("error"));
    CHECK_FALSE(save->get_sensitive());
    // A direct response must obey validation too, not just the button state.
    dialog.response(Gtk::ResponseType::OK);
    drainGtkEvents();
    CHECK(rt::test::runtimeTrackSpec(runtime, trackId).title == "Date edit");
    CHECK(rt::test::runtimeTrackSpec(runtime, trackId).recordingDate ==
          library::RecordingDate{.year = 1955, .month = 6});

    date->set_text(" 1981 ");
    CHECK_FALSE(date->has_css_class("error"));
    REQUIRE(save->get_sensitive());
    emitClicked(*save);
    REQUIRE(tryPumpGtkEventsUntil(
      [&]
      {
        auto const spec = rt::test::runtimeTrackSpec(runtime, trackId);
        return spec.title == "Changed" && spec.recordingDate == library::RecordingDate{.year = 1981};
      }));
    dialog.close();
    parent.close();
    drainGtkEvents();
  }

  TEST_CASE("TrackPropertiesDialog - mixed recording date remains unchanged during another field edit",
            "[gtk][integration][tag][dialog][recording-date]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto ids = std::vector<TrackId>{};
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        {
                          ids.push_back(library::test::addTrackWithUniqueFixtureUri(
                            musicLibrary, {.title = "Common", .recordingDate = {.year = 1955}}));
                          ids.push_back(library::test::addTrackWithUniqueFixtureUri(
                            musicLibrary, {.title = "Common", .recordingDate = {.year = 1955, .month = 6}}));
                        }};
    auto& runtime = fixture.runtime();
    auto const& catalog = ao::test::englishMessageCatalog();
    auto cache = TrackRowCache{runtime.library(), catalog};
    auto parent = Gtk::Window{};
    auto dialog =
      TrackPropertiesDialog{parent, runtime.async(), runtime.library(), runtime.completion(), catalog, cache, ids};
    parent.present();
    dialog.present();
    drainGtkEvents();
    entryWithText(dialog, "Common")->set_text("Changed");
    auto* const save = findButtonByLabel(dialog, "Save");
    REQUIRE(save != nullptr);
    REQUIRE(save->get_sensitive());
    emitClicked(*save);
    REQUIRE(tryPumpGtkEventsUntil(
      [&]
      {
        return std::ranges::all_of(
          ids, [&](TrackId id) { return rt::test::runtimeTrackSpec(runtime, id).title == "Changed"; });
      }));
    CHECK(rt::test::runtimeTrackSpec(runtime, ids[0]).recordingDate == library::RecordingDate{.year = 1955});
    CHECK(rt::test::runtimeTrackSpec(runtime, ids[1]).recordingDate ==
          library::RecordingDate{.year = 1955, .month = 6});
    dialog.close();
    parent.close();
    drainGtkEvents();
  }
} // namespace ao::gtk::test
