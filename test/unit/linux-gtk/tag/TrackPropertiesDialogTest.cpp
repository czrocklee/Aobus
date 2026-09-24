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
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>

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
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    class [[nodiscard]] FinalizationObserver final
    {
    public:
      explicit FinalizationObserver(::GObject* const object) { ::g_weak_ref_init(&_weakRef, object); }
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
      ::GWeakRef _weakRef{};
    };
  } // namespace

  TEST_CASE("TrackPropertiesDialog - pending Save freezes the draft and commits before closing",
            "[gtk][integration][tag][dialog][concurrency]")
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

    REQUIRE(std::ranges::contains(Gtk::Window::list_toplevels(), dialog));
    auto const spinButtons = collectAll<Gtk::SpinButton>(*dialog);
    REQUIRE_FALSE(spinButtons.empty());
    CHECK_FALSE(saveButton->get_sensitive());
    CHECK((*titleEntryIter)->get_text().raw() == "After Save");
    CHECK(std::ranges::none_of(entries, [](Gtk::Entry const* entry) { return entry->get_sensitive(); }));
    CHECK(std::ranges::none_of(entries, [](Gtk::Entry const* entry) { return entry->get_editable(); }));
    CHECK(std::ranges::none_of(spinButtons, [](Gtk::SpinButton const* spin) { return spin->get_sensitive(); }));
    CHECK(std::ranges::none_of(spinButtons, [](Gtk::SpinButton const* spin) { return spin->get_editable(); }));

    REQUIRE(tryPumpGtkEventsUntil([&runtime, trackId]
                                  { return rt::test::runtimeTrackSpec(runtime, trackId).title == "After Save"; }));
    CHECK(tryPumpGtkEventsUntil([&finalization] { return finalization.isFinalized(); }, std::chrono::seconds{2}));

    parent.close();
    drainGtkEvents();
  }

  TEST_CASE("TrackPropertiesDialog - stale managed Save keeps the editor open",
            "[gtk][integration][tag][dialog][async]")
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

    REQUIRE(std::ranges::contains(Gtk::Window::list_toplevels(), dialog));
    CHECK(dialog->get_visible());
    CHECK((*titleEntryIter)->get_text().raw() == "Stale Replacement");
    CHECK_FALSE((*titleEntryIter)->get_sensitive());
    CHECK_FALSE((*titleEntryIter)->get_editable());
    CHECK(rt::test::runtimeTrackSpec(runtime, trackId).title == "External Change");

    staleDialog->response(Gtk::ResponseType::CLOSE);
    dialog->response(Gtk::ResponseType::CLOSE);
    parent.close();
    drainGtkEvents();
  }

  TEST_CASE("TrackPropertiesDialog - post-publication owner teardown does not cancel an admitted Save",
            "[gtk][integration][tag][dialog][concurrency]")
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

  namespace
  {
    Gtk::Entry* findEntryByText(Gtk::Widget& root, std::string_view const text)
    {
      auto const entries = collectAll<Gtk::Entry>(root);
      auto const iter =
        std::ranges::find_if(entries, [text](Gtk::Entry const* entry) { return entry->get_text().raw() == text; });
      return iter != entries.end() ? *iter : nullptr;
    }

    Gtk::Entry* findEntryForLabel(Gtk::Widget& root, std::string const& labelText)
    {
      auto* const label = findLabelByText(root, labelText);

      if (label == nullptr || label->get_parent() == nullptr)
      {
        return nullptr;
      }

      return findWidget<Gtk::Entry>(*label->get_parent());
    }

    AppDialog* findOtherDialogWithText(Gtk::Window const& owner, std::string const& text)
    {
      for (auto* const topLevel : Gtk::Window::list_toplevels())
      {
        if (topLevel != &owner && findLabelByText(*topLevel, text) != nullptr)
        {
          return dynamic_cast<AppDialog*>(topLevel);
        }
      }

      return nullptr;
    }
  } // namespace

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

      auto* const titleEntry = findEntryByText(dialog, "Track 1");
      REQUIRE(titleEntry != nullptr);

      titleEntry->set_text("Renamed Track");
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

      auto* const titleEntry = findEntryForLabel(dialog, "Title");
      auto* const artistEntry = findEntryForLabel(dialog, "Artist");
      auto* const albumEntry = findEntryForLabel(dialog, "Album");
      REQUIRE(titleEntry != nullptr);
      REQUIRE(artistEntry != nullptr);
      REQUIRE(albumEntry != nullptr);
      CHECK(titleEntry->get_text().empty());
      CHECK(titleEntry->get_placeholder_text().raw() == mixedText);
      CHECK_FALSE(titleEntry->get_sensitive());
      CHECK(artistEntry->get_text().empty());
      CHECK(artistEntry->get_placeholder_text().raw() == mixedText);
      CHECK_FALSE(artistEntry->get_sensitive());
      CHECK(albumEntry->get_text() == "Album 1");
      CHECK(albumEntry->get_sensitive());
      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(saveButton != nullptr);
      CHECK_FALSE(saveButton->get_sensitive());
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
  }

  TEST_CASE("TrackPropertiesDialog - keeps a presented draft until save settlement",
            "[gtk][integration][tag][dialog][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                                     {
                                       trackId = library::test::addTrackWithUniqueFixtureUri(
                                         musicLibrary, {.title = "Before save", .artist = "Settlement Artist"});
                                     }};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto window = Gtk::Window{};

    REQUIRE(trackId != kInvalidTrackId);

    SECTION("accepted save keeps the exact pending draft and closes only after readback is committed")
    {
      auto dialog = TrackPropertiesDialog{window,
                                          runtime.async(),
                                          runtime.library(),
                                          runtime.completion(),
                                          ao::test::englishMessageCatalog(),
                                          cache,
                                          {trackId}};
      dialog.present();
      drainGtkEvents();
      REQUIRE(dialog.get_visible());

      auto* const titleEntry = findEntryByText(dialog, "Before save");
      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(titleEntry != nullptr);
      REQUIRE(saveButton != nullptr);
      REQUIRE(tryPumpGtkEventsUntil([&dialog] { return dialog.get_mapped(); }));
      auto* const yearLabel = findLabelByText(dialog, "Year");
      REQUIRE(yearLabel != nullptr);
      REQUIRE(yearLabel->get_parent() != nullptr);
      auto* const yearEditor = findWidget<Gtk::SpinButton>(*yearLabel->get_parent());
      REQUIRE(yearEditor != nullptr);
      REQUIRE(titleEntry->is_sensitive());
      REQUIRE(titleEntry->get_editable());
      REQUIRE(yearEditor->is_sensitive());
      REQUIRE(yearEditor->get_editable());

      titleEntry->set_text("Settled rename");
      yearEditor->set_value(2027);
      REQUIRE(saveButton->get_sensitive());
      bool textUnlocked = false;
      bool numberUnlocked = false;
      auto textObserver = sigc::scoped_connection{titleEntry->property_editable().signal_changed().connect(
        [&] { textUnlocked = textUnlocked || titleEntry->get_editable(); })};
      auto numberObserver = sigc::scoped_connection{yearEditor->property_editable().signal_changed().connect(
        [&] { numberUnlocked = numberUnlocked || yearEditor->get_editable(); })};
      emitClicked(*saveButton); // Public Save binding; not a native pointer-delivery claim.

      CHECK(dialog.get_visible());
      CHECK(dialog.get_mapped());
      CHECK(titleEntry->get_text() == "Settled rename");
      CHECK_FALSE(saveButton->get_sensitive());
      CHECK_FALSE(titleEntry->is_sensitive());
      CHECK_FALSE(titleEntry->get_editable());
      CHECK_FALSE(yearEditor->is_sensitive());
      CHECK_FALSE(yearEditor->get_editable());
      REQUIRE(tryPumpGtkEventsUntil([&dialog] { return !dialog.get_visible(); }));
      CHECK_FALSE(textUnlocked);
      CHECK_FALSE(numberUnlocked);

      auto scope = runtime.library().snapshot();
      auto const title = scope.trackField(trackId, rt::TrackField::Title);
      auto const year = scope.trackField(trackId, rt::TrackField::Year);
      REQUIRE(std::holds_alternative<std::string>(title));
      REQUIRE(std::holds_alternative<std::uint16_t>(year));
      CHECK(std::get<std::string>(title) == "Settled rename");
      CHECK(std::get<std::uint16_t>(year) == 2027);
    }

    SECTION("repeated Save preserves the admitted draft without starting another submission")
    {
      auto dialog = TrackPropertiesDialog{window,
                                          runtime.async(),
                                          runtime.library(),
                                          runtime.completion(),
                                          ao::test::englishMessageCatalog(),
                                          cache,
                                          {trackId}};
      dialog.present();
      drainGtkEvents();

      auto* const titleEntry = findEntryByText(dialog, "Before save");
      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(titleEntry != nullptr);
      REQUIRE(saveButton != nullptr);
      REQUIRE(tryPumpGtkEventsUntil([&dialog] { return dialog.get_mapped(); }));
      titleEntry->set_text("Single draft");
      std::size_t createdWindowCount = 0;
      auto const topLevelsPtr = Gtk::Window::get_toplevels();
      // Observe transient creation even if the successful close destroys it.
      auto const topLevelsConnection = sigc::scoped_connection{topLevelsPtr->signal_items_changed().connect(
        [&createdWindowCount](::guint, ::guint, ::guint const added) { createdWindowCount += added; })};

      emitClicked(*saveButton);
      emitClicked(*saveButton); // Controlled callback re-entry; the insensitive button blocks real users.
      CHECK(dialog.get_visible());
      CHECK(dialog.get_mapped());
      CHECK(titleEntry->get_text() == "Single draft");
      CHECK_FALSE(titleEntry->is_sensitive());
      CHECK_FALSE(titleEntry->get_editable());

      REQUIRE(tryPumpGtkEventsUntil([&dialog] { return !dialog.get_visible(); }));
      CHECK(createdWindowCount == 0);

      auto scope = runtime.library().snapshot();
      auto const title = scope.trackField(trackId, rt::TrackField::Title);
      REQUIRE(std::holds_alternative<std::string>(title));
      CHECK(std::get<std::string>(title) == "Single draft");
    }

    SECTION("stale settlement keeps the exact submitted draft open with its error")
    {
      auto dialog = TrackPropertiesDialog{window,
                                          runtime.async(),
                                          runtime.library(),
                                          runtime.completion(),
                                          ao::test::englishMessageCatalog(),
                                          cache,
                                          {trackId}};
      dialog.present();
      drainGtkEvents();

      auto* const titleEntry = findEntryByText(dialog, "Before save");
      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(titleEntry != nullptr);
      REQUIRE(saveButton != nullptr);
      REQUIRE(tryPumpGtkEventsUntil([&dialog] { return dialog.get_mapped(); }));
      titleEntry->set_text("Unavailable draft");

      REQUIRE(runGtkTask(
        runtime, runtime.library().commands().createListAsync(rt::ListDraft{.name = "Invalidate properties"})));
      emitClicked(*saveButton); // Controlled binding emission after the session became stale.
      CHECK(dialog.get_visible());
      CHECK(dialog.get_mapped());
      CHECK(titleEntry->get_text() == "Unavailable draft");
      CHECK_FALSE(titleEntry->is_sensitive());
      CHECK_FALSE(titleEntry->get_editable());

      AppDialog* errorDialog = nullptr;
      REQUIRE(tryPumpGtkEventsUntil(
        [&]
        {
          errorDialog = findOtherDialogWithText(
            dialog, "The library changed while this dialog was open. Reload the properties and try again.");
          return errorDialog != nullptr;
        }));
      CHECK(dialog.get_visible());
      CHECK(dialog.get_mapped());
      CHECK(titleEntry->get_text() == "Unavailable draft");
      CHECK_FALSE(titleEntry->is_sensitive());
      CHECK_FALSE(titleEntry->get_editable());
      CHECK_FALSE(saveButton->get_sensitive());
      auto scope = runtime.library().snapshot();
      auto const title = scope.trackField(trackId, rt::TrackField::Title);
      REQUIRE(std::holds_alternative<std::string>(title));
      CHECK(std::get<std::string>(title) == "Before save");

      errorDialog->response(Gtk::ResponseType::CLOSE);
      dialog.response(Gtk::ResponseType::CLOSE);
      drainGtkEvents();
    }
  }
} // namespace ao::gtk::test
