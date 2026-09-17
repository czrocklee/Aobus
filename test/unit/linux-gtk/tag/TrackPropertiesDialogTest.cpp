// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "app/AppDialog.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <giomm/listmodel.h>
#include <glib-object.h>
#include <glibmm/main.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    class [[nodiscard]] FinalizationObserver final
    {
    public:
      explicit FinalizationObserver(GObject* const object) { ::g_weak_ref_init(&_weakRef, object); }
      ~FinalizationObserver() { ::g_weak_ref_clear(&_weakRef); }

      FinalizationObserver(FinalizationObserver const&) = delete;
      FinalizationObserver& operator=(FinalizationObserver const&) = delete;
      FinalizationObserver(FinalizationObserver&&) = delete;
      FinalizationObserver& operator=(FinalizationObserver&&) = delete;

      bool isFinalized()
      {
        auto* const object = ::g_weak_ref_get(&_weakRef);

        if (object == nullptr)
        {
          return true;
        }

        ::g_object_unref(object);
        return false;
      }

    private:
      GWeakRef _weakRef{};
    };
  } // namespace

  TEST_CASE("TrackPropertiesDialog - pending Save freezes the draft and commits before closing",
            "[gtk][regression][dialog][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Before Save"}); }};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto parent = Gtk::Window{};
    parent.present();

    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(parent,
                                                                  runtime.async(),
                                                                  runtime.library(),
                                                                  runtime.completion(),
                                                                  ao::test::englishMessageCatalog(),
                                                                  cache,
                                                                  std::vector{trackId});
    dialog->present();
    drainGtkEvents();

    auto const entries = collectAll<Gtk::Entry>(*dialog);
    auto const titleEntryIter =
      std::ranges::find_if(entries, [](Gtk::Entry const* entry) { return entry->get_text().raw() == "Before Save"; });
    REQUIRE(titleEntryIter != entries.end());
    (*titleEntryIter)->set_text("After Save");
    auto* const saveButton = findButtonByLabel(*dialog, "Save");
    REQUIRE(saveButton != nullptr);
    auto finalization = FinalizationObserver{G_OBJECT(dialog->gobj())};

    emitClicked(*saveButton);

    auto const spinButtons = collectAll<Gtk::SpinButton>(*dialog);
    CHECK_FALSE(saveButton->get_sensitive());
    CHECK((*titleEntryIter)->get_text().raw() == "After Save");
    CHECK(std::ranges::none_of(entries, [](Gtk::Entry const* entry) { return entry->get_sensitive(); }));
    CHECK(std::ranges::none_of(spinButtons, [](Gtk::SpinButton const* spin) { return spin->get_sensitive(); }));

    REQUIRE(tryPumpGtkEventsUntil([&runtime, trackId]
                                  { return rt::test::runtimeTrackSpec(runtime, trackId).title == "After Save"; }));
    CHECK(tryPumpGtkEventsUntil([&finalization] { return finalization.isFinalized(); }, std::chrono::seconds{2}));

    parent.close();
    drainGtkEvents();
  }

  TEST_CASE("TrackPropertiesDialog - stale managed Save keeps the editor open", "[gtk][regression][tag][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Stale Save"}); }};
    auto& runtime = fixture.runtime();
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto cache = TrackRowCache{runtime.library(), textCatalog};
    auto parent = Gtk::Window{};
    parent.present();
    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(
      parent, runtime.async(), runtime.library(), runtime.completion(), textCatalog, cache, std::vector{trackId});
    dialog->present();
    drainGtkEvents();

    auto const entries = collectAll<Gtk::Entry>(*dialog);
    auto const titleEntryIter =
      std::ranges::find_if(entries, [](Gtk::Entry const* entry) { return entry->get_text().raw() == "Stale Save"; });
    REQUIRE(titleEntryIter != entries.end());
    (*titleEntryIter)->set_text("Stale Replacement");
    updateRuntimeTrack(runtime, trackId, [](library::test::TrackSpec& spec) { spec.title = "External Change"; });

    dialog->response(Gtk::ResponseType::OK);

    AppDialog* staleDialog = nullptr;
    auto const staleMessage = std::string{i18n::requiredText(textCatalog, i18n::MessageId::GtkTrackSaveStale)};
    REQUIRE(tryPumpGtkEventsUntil(
      [&staleDialog, dialog, &staleMessage]
      {
        for (auto* const topLevel : Gtk::Window::list_toplevels())
        {
          if (topLevel == dialog || findLabelByText(*topLevel, staleMessage) == nullptr)
          {
            continue;
          }

          staleDialog = dynamic_cast<AppDialog*>(topLevel);
          return staleDialog != nullptr;
        }

        return false;
      }));

    CHECK(std::ranges::contains(Gtk::Window::list_toplevels(), dialog));
    CHECK(dialog->get_visible());
    CHECK((*titleEntryIter)->get_text().raw() == "Stale Replacement");
    CHECK_FALSE((*titleEntryIter)->get_sensitive());
    CHECK(rt::test::runtimeTrackSpec(runtime, trackId).title == "External Change");

    staleDialog->response(Gtk::ResponseType::CLOSE);
    dialog->response(Gtk::ResponseType::CLOSE);
    parent.close();
    drainGtkEvents();
  }

  TEST_CASE("TrackPropertiesDialog - post-publication owner teardown does not cancel an admitted Save",
            "[gtk][regression][dialog][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Owner Before"}); }};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto parentPtr = std::make_unique<Gtk::Window>();
    parentPtr->present();
    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(*parentPtr,
                                                                  runtime.async(),
                                                                  runtime.library(),
                                                                  runtime.completion(),
                                                                  ao::test::englishMessageCatalog(),
                                                                  cache,
                                                                  std::vector{trackId});
    dialog->present();
    drainGtkEvents();

    auto const entries = collectAll<Gtk::Entry>(*dialog);
    auto const titleEntryIter =
      std::ranges::find_if(entries, [](Gtk::Entry const* entry) { return entry->get_text().raw() == "Owner Before"; });
    REQUIRE(titleEntryIter != entries.end());
    (*titleEntryIter)->set_text("Owner After");
    auto* const saveButton = findButtonByLabel(*dialog, "Save");
    REQUIRE(saveButton != nullptr);

    bool commitPublished = false;
    auto destroyOwnerConnection = sigc::scoped_connection{};
    [[maybe_unused]] auto const changeSub = runtime.library().changes().onChanged(
      [&](rt::LibraryChangeSet const& changeSet)
      {
        if (std::ranges::contains(changeSet.tracksMutated, trackId))
        {
          commitPublished = true;
          destroyOwnerConnection = Glib::signal_idle().connect(
            [&parentPtr]
            {
              parentPtr.reset();
              return false;
            });
        }
      });

    emitClicked(*saveButton);

    REQUIRE(tryPumpGtkEventsUntil([&commitPublished, &parentPtr] { return commitPublished && !parentPtr; }));
    CHECK(rt::test::runtimeTrackSpec(runtime, trackId).title == "Owner After");
    CHECK_FALSE(std::ranges::contains(Gtk::Window::list_toplevels(), dialog));
    drainGtkEvents();
  }

  TEST_CASE("TrackPropertiesDialog - renders metadata fields for the selected tracks", "[gtk][unit][tag][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId1 = kInvalidTrackId;
    auto trackId2 = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        {
                          trackId1 = library::test::addTrackWithUniqueFixtureUri(musicLibrary,
                                                                                 {.title = "Track 1",
                                                                                  .artist = "Artist 1",
                                                                                  .album = "Album 1",
                                                                                  .albumArtist = "AA",
                                                                                  .genre = "Rock",
                                                                                  .uri = "music/track1.flac",
                                                                                  .year = 2023,
                                                                                  .discNumber = 1,
                                                                                  .trackNumber = 1,
                                                                                  .duration = std::chrono::seconds{1},
                                                                                  .bitrate = Bitrate{320},
                                                                                  .sampleRate = SampleRate{44100},
                                                                                  .channels = Channels{2},
                                                                                  .bitDepth = BitDepth{16},
                                                                                  .codec = AudioCodec::Flac});
                          trackId2 = library::test::addTrackWithUniqueFixtureUri(musicLibrary,
                                                                                 {.title = "Track 2",
                                                                                  .artist = "Artist 2",
                                                                                  .album = "Album 1",
                                                                                  .albumArtist = "AA",
                                                                                  .genre = "Rock",
                                                                                  .uri = "music/track2.flac",
                                                                                  .year = 2023,
                                                                                  .discNumber = 1,
                                                                                  .trackNumber = 2,
                                                                                  .duration = std::chrono::seconds{2},
                                                                                  .bitrate = Bitrate{320},
                                                                                  .sampleRate = SampleRate{48000},
                                                                                  .channels = Channels{2},
                                                                                  .bitDepth = BitDepth{24},
                                                                                  .codec = AudioCodec::Flac});
                        }};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto window = Gtk::Window{};

    REQUIRE(trackId1 != kInvalidTrackId);
    REQUIRE(trackId2 != kInvalidTrackId);

    SECTION("dialog creation and data loading")
    {
      auto dialog = TrackPropertiesDialog{window,
                                          runtime.async(),
                                          runtime.library(),
                                          runtime.completion(),
                                          ao::test::englishMessageCatalog(),
                                          cache,
                                          {trackId1}};
      drainGtkEvents();

      auto const entries = collectAll<Gtk::Entry>(dialog);
      CHECK_FALSE(entries.empty());

      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(saveButton != nullptr);
      CHECK_FALSE(saveButton->get_sensitive());

      auto const titleEntryIter =
        std::ranges::find_if(entries, [](Gtk::Entry const* entry) { return entry->get_text().raw() == "Track 1"; });
      REQUIRE(titleEntryIter != entries.end());

      (*titleEntryIter)->set_text("Renamed Track");
      drainGtkEvents();
      CHECK(saveButton->get_sensitive());
    }

    SECTION("multi-track selection marks differing fields as mixed")
    {
      auto const& textCatalog = ao::test::englishMessageCatalog();
      auto dialog = TrackPropertiesDialog{
        window, runtime.async(), runtime.library(), runtime.completion(), textCatalog, cache, {trackId1, trackId2}};
      drainGtkEvents();

      CHECK(dialog.get_title() == "Properties — 2 tracks selected");

      // Title and artist differ across the two tracks. UIModel owns the mixed-state decision; this
      // adapter test only asserts that the dialog reflects that row view in GTK widgets.
      auto const entries = collectAll<Gtk::Entry>(dialog);
      auto const mixedText = i18n::requiredText(textCatalog, i18n::MessageId::TrackMultipleValues);
      auto const mixedCount = std::ranges::count_if(
        entries, [mixedText](Gtk::Entry const* entry) { return entry->get_placeholder_text().raw() == mixedText; });
      CHECK(mixedCount >= 1);
    }

    SECTION("an incomplete selection shows no partial writable baseline")
    {
      auto const targetIds = GENERATE_COPY(std::vector{TrackId{999999}}, std::vector{trackId1, TrackId{999999}});
      auto dialog = TrackPropertiesDialog{window,
                                          runtime.async(),
                                          runtime.library(),
                                          runtime.completion(),
                                          ao::test::englishMessageCatalog(),
                                          cache,
                                          targetIds};

      auto const labels = collectAll<Gtk::Label>(dialog);
      auto const errorLabelIter = std::ranges::find_if(
        labels, [](Gtk::Label const* label) { return label->has_css_class("ao-properties-session-error"); });
      REQUIRE(errorLabelIter != labels.end());
      CHECK((*errorLabelIter)->get_visible());
      CHECK((*errorLabelIter)->get_text().raw().contains("Track authoring target not found"));

      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(saveButton != nullptr);
      CHECK_FALSE(saveButton->get_sensitive());

      auto const entries = collectAll<Gtk::Entry>(dialog);
      CHECK(
        std::ranges::none_of(entries, [](Gtk::Entry const* entry) { return entry->get_text().raw() == "Track 1"; }));
      CHECK(std::ranges::none_of(entries, [](Gtk::Entry const* entry) { return entry->get_sensitive(); }));
    }

    SECTION("a repeated Save while submitting is rejected")
    {
      auto dialog = TrackPropertiesDialog{window,
                                          runtime.async(),
                                          runtime.library(),
                                          runtime.completion(),
                                          ao::test::englishMessageCatalog(),
                                          cache,
                                          {trackId1}};
      dialog.present();
      drainGtkEvents();

      auto const entries = collectAll<Gtk::Entry>(dialog);
      auto const titleEntryIter =
        std::ranges::find_if(entries, [](Gtk::Entry const* entry) { return entry->get_text().raw() == "Track 1"; });
      REQUIRE(titleEntryIter != entries.end());
      (*titleEntryIter)->set_text("One submitted draft");
      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(saveButton != nullptr);
      std::size_t createdWindowCount = 0;
      auto const topLevelsPtr = Gtk::Window::get_toplevels();
      // The list model records creation before a transient can be destroyed with its owner.
      [[maybe_unused]] auto const topLevelsConnection =
        sigc::scoped_connection{topLevelsPtr->signal_items_changed().connect(
          [&createdWindowCount](::guint, ::guint, ::guint const added) { createdWindowCount += added; })};

      emitClicked(*saveButton);
      // Reflection deliberately bypasses GTK sensitivity to exercise the handler guard.
      emitClicked(*saveButton);

      REQUIRE(tryPumpGtkEventsUntil(
        [&runtime, trackId1] { return rt::test::runtimeTrackSpec(runtime, trackId1).title == "One submitted draft"; }));
      CHECK(tryPumpGtkEventsUntil([&dialog] { return !dialog.get_visible(); }));
      CHECK(createdWindowCount == 0);
      CHECK(rt::test::runtimeTrackSpec(runtime, trackId1).title == "One submitted draft");
    }
  }
} // namespace ao::gtk::test
