// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "app/AppDialog.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
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
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <algorithm>
#include <array>
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

  TEST_CASE("TrackPropertiesDialog - pending Save freezes the draft and commits before closing", "[concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Before Save"}); }};
    auto& runtime = fixture.runtime();
    auto parent = Gtk::Window{};
    parent.present();

    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(parent,
                                                                  runtime.async(),
                                                                  fixture.editing(),
                                                                  runtime.completion(),
                                                                  ao::test::englishMessageCatalog(),
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

  TEST_CASE("TrackPropertiesDialog - stale managed Save keeps the editor open")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Stale Save"}); }};
    auto& runtime = fixture.runtime();
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto parent = Gtk::Window{};
    parent.present();
    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(
      parent, runtime.async(), fixture.editing(), runtime.completion(), textCatalog, std::vector{trackId});
    dialog->present();
    drainGtkEvents();

    auto const entries = collectAll<Gtk::Entry>(*dialog);
    auto const titleEntryIter =
      std::ranges::find_if(entries, [](Gtk::Entry const* entry) { return entry->get_text().raw() == "Stale Save"; });
    REQUIRE(titleEntryIter != entries.end());
    (*titleEntryIter)->set_text("Stale Replacement");
    updateRuntimeTrack(
      fixture.owner(), trackId, [](library::test::TrackSpec& spec) { spec.title = "External Change"; });

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

  TEST_CASE("TrackPropertiesDialog - post-publication owner teardown does not cancel an admitted Save", "[concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Owner Before"}); }};
    auto& runtime = fixture.runtime();
    auto parentPtr = std::make_unique<Gtk::Window>();
    parentPtr->present();
    auto* const dialog = Gtk::make_managed<TrackPropertiesDialog>(*parentPtr,
                                                                  runtime.async(),
                                                                  fixture.editing(),
                                                                  runtime.completion(),
                                                                  ao::test::englishMessageCatalog(),
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

    std::string clearFieldLabel(i18n::MessageCatalog const& catalog, i18n::MessageId field, std::size_t const count)
    {
      return i18n::requiredFormat(catalog,
                                  i18n::MessageId::TrackClearFieldForTracks,
                                  {{"field", i18n::requiredText(catalog, field)}, {"count", count}});
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

  TEST_CASE("TrackPropertiesDialog - renders metadata fields for the selected tracks")
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
    auto const& catalog = ao::test::englishMessageCatalog();
    auto window = Gtk::Window{};

    REQUIRE(trackId1 != kInvalidTrackId);
    REQUIRE(trackId2 != kInvalidTrackId);

    SECTION("dialog creation and data loading")
    {
      auto dialog = TrackPropertiesDialog{
        window, runtime.async(), fixture.editing(), runtime.completion(), catalog, std::array{trackId1}};
      drainGtkEvents();

      CHECK(findButtonByLabel(dialog, clearFieldLabel(catalog, i18n::MessageId::TrackFieldAlbum, 1)) == nullptr);
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
        window, runtime.async(), fixture.editing(), runtime.completion(), textCatalog, std::array{trackId1, trackId2}};
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
      CHECK(titleEntry->get_sensitive());
      CHECK(artistEntry->get_text().empty());
      CHECK(artistEntry->get_placeholder_text().raw() == mixedText);
      CHECK(artistEntry->get_sensitive());
      CHECK(albumEntry->get_text() == "Album 1");
      CHECK(albumEntry->get_sensitive());
      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(saveButton != nullptr);
      CHECK_FALSE(saveButton->get_sensitive());
    }

    SECTION("two occurrences of the same track use batch editing rules")
    {
      auto dialog = TrackPropertiesDialog{window,
                                          runtime.async(),
                                          fixture.editing(),
                                          runtime.completion(),
                                          ao::test::englishMessageCatalog(),
                                          std::array{trackId1, trackId1}};
      CHECK(dialog.get_title() == "Properties — 2 tracks selected");
      auto* const title = findEntryForLabel(dialog, "Title");
      auto* const save = findButtonByLabel(dialog, "Save");
      auto* const clear = findButtonByLabel(dialog, "Clear Title for 2 tracks");
      REQUIRE(title != nullptr);
      REQUIRE(save != nullptr);
      REQUIRE(clear != nullptr);
      title->set_text("");
      CHECK_FALSE(save->get_sensitive());
      emitClicked(*clear);
      CHECK(save->get_sensitive());
    }

    SECTION("an unbound selection shows no partial writable baseline or Title Clear")
    {
      auto const targetIds =
        GENERATE_COPY(std::vector<TrackId>{}, std::vector{TrackId{999999}}, std::vector{trackId1, TrackId{999999}});
      auto dialog =
        TrackPropertiesDialog{window, runtime.async(), fixture.editing(), runtime.completion(), catalog, targetIds};
      CHECK(dialog.get_title() == (targetIds.size() > 1 ? "Properties — 2 tracks selected" : "Properties"));
      CHECK(findButtonByLabel(dialog, clearFieldLabel(catalog, i18n::MessageId::TrackFieldTitle, 0)) == nullptr);
      CHECK(findButtonByLabel(dialog, clearFieldLabel(catalog, i18n::MessageId::TrackFieldTitle, 1)) == nullptr);
      CHECK(findButtonByLabel(dialog, clearFieldLabel(catalog, i18n::MessageId::TrackFieldTitle, 2)) == nullptr);

      auto const labels = collectAll<Gtk::Label>(dialog);
      auto const errorLabelIter = std::ranges::find_if(
        labels, [](Gtk::Label const* label) { return label->has_css_class("ao-properties-session-error"); });
      REQUIRE(errorLabelIter != labels.end());
      CHECK((*errorLabelIter)->get_visible());
      auto const expectedError = std::string_view{targetIds.empty() ? "Cannot bind an empty track target set"
                                                                    : "Track authoring target not found"};
      CHECK((*errorLabelIter)->get_text().raw().contains(expectedError));

      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(saveButton != nullptr);
      CHECK_FALSE(saveButton->get_sensitive());

      auto const entries = collectAll<Gtk::Entry>(dialog);
      CHECK(
        std::ranges::none_of(entries, [](Gtk::Entry const* entry) { return entry->get_text().raw() == "Track 1"; }));
      CHECK(std::ranges::none_of(entries, [](Gtk::Entry const* entry) { return entry->get_sensitive(); }));
    }
  }

  TEST_CASE("TrackPropertiesDialog - named Title Clear records ordinary blanks only for the saved selection")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto firstId = kInvalidTrackId;
    auto secondId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& storage)
      {
        firstId = library::test::addTrackWithUniqueFixtureUri(storage, {.title = "", .album = "First"});
        secondId = library::test::addTrackWithUniqueFixtureUri(storage, {.title = "", .album = "Second"});
      }};
    auto const ids = std::vector{firstId, secondId};
    auto const targetIds = GENERATE_COPY(std::vector{firstId}, std::vector{secondId}, ids);
    auto& runtime = fixture.runtime();
    auto const& catalog = ao::test::englishMessageCatalog();
    using i18n::MessageId;
    auto const wouldRecordClear = [](TrackId id)
    {
      return rt::TrackChangeRecord{.trackId = id,
                                   .fields = {{.field = "title",
                                               .oldValue = "",
                                               .newValue = "",
                                               .optReason = rt::TrackFieldChangeReason::ExplicitClearRecorded}}};
    };
    // Only an ordinary blank still needs an explicit clear; the displayed text cannot distinguish them.
    auto const checkClearPreview = [&](std::vector<rt::TrackChangeRecord> const& expected)
    {
      auto const previewRes =
        runGtkTask(runtime, fixture.owner().libraryCommands().previewUpdateMetadataAsync(ids, {.optTitle = ""}));
      REQUIRE(previewRes);
      CHECK(previewRes->changes == expected);
    };
    auto const ordinaryTitles = std::vector{wouldRecordClear(firstId), wouldRecordClear(secondId)};
    checkClearPreview(ordinaryTitles);

    auto parent = Gtk::Window{};
    auto dialog =
      TrackPropertiesDialog{parent, runtime.async(), fixture.editing(), runtime.completion(), catalog, targetIds};
    dialog.present();
    drainGtkEvents();
    auto const label = clearFieldLabel(catalog, MessageId::TrackFieldTitle, targetIds.size());
    auto* const clear = dynamic_cast<Gtk::ToggleButton*>(findButtonByLabel(dialog, label));
    auto* const save = findButtonByLabel(dialog, std::string{i18n::requiredText(catalog, MessageId::CommonSave)});
    auto* const title = findEntryForLabel(dialog, std::string{i18n::requiredText(catalog, MessageId::TrackFieldTitle)});
    REQUIRE(clear != nullptr);
    REQUIRE(save != nullptr);
    REQUIRE(title != nullptr);
    CHECK(hasAccessibleLabel(*clear, label));
    REQUIRE(clear->is_sensitive());
    CHECK(title->get_text().empty());
    CHECK_FALSE(save->is_sensitive());

    emitClicked(*clear);
    CHECK(save->is_sensitive());
    emitClicked(*clear);
    CHECK_FALSE(save->is_sensitive());
    CHECK(title->get_editable());
    checkClearPreview(ordinaryTitles);

    emitClicked(*clear);
    REQUIRE(save->is_sensitive());
    emitClicked(*save);
    REQUIRE(tryPumpGtkEventsUntil([&dialog] { return !dialog.get_visible(); }));

    if (targetIds.size() == 1)
    {
      auto const unselectedId = targetIds.front() == firstId ? secondId : firstId;
      checkClearPreview({wouldRecordClear(unselectedId)});
    }
    else
    {
      checkClearPreview({});
    }
  }

  TEST_CASE("TrackPropertiesDialog - batch scalar actions preserve, replace or clear both bound tracks")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto ids = std::vector<TrackId>{};
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& storage)
                                     {
                                       ids.push_back(library::test::addTrackWithUniqueFixtureUri(
                                         storage, {.title = "", .album = "First", .genre = "Jazz", .year = 1960}));
                                       ids.push_back(library::test::addTrackWithUniqueFixtureUri(
                                         storage, {.title = "", .album = "Second", .genre = "Jazz", .year = 1970}));
                                     }};
    auto& runtime = fixture.runtime();
    auto const& catalog = ao::test::englishMessageCatalog();
    auto parent = Gtk::Window{};
    auto dialog = TrackPropertiesDialog{parent, runtime.async(), fixture.editing(), runtime.completion(), catalog, ids};
    dialog.present();
    drainGtkEvents();
    using i18n::MessageId;
    auto const fieldLabel = [&](MessageId id) { return std::string{i18n::requiredText(catalog, id)}; };
    auto const clearLabel = [&](MessageId id) { return clearFieldLabel(catalog, id, 2); };
    auto* const save = findButtonByLabel(dialog, fieldLabel(MessageId::CommonSave));
    auto* const album = findEntryForLabel(dialog, fieldLabel(MessageId::TrackFieldAlbum));
    auto* const genre = findEntryForLabel(dialog, fieldLabel(MessageId::TrackFieldGenre));
    REQUIRE(save != nullptr);
    REQUIRE(album != nullptr);
    REQUIRE(genre != nullptr);
    auto checkTracks = [&](std::string_view firstAlbum, std::string_view secondAlbum, std::string_view expectedGenre)
    {
      auto const first = rt::test::runtimeTrackSpec(runtime, ids.front());
      auto const second = rt::test::runtimeTrackSpec(runtime, ids.back());
      CHECK(first.album == firstAlbum);
      CHECK(second.album == secondAlbum);
      CHECK(first.genre == expectedGenre);
      CHECK(second.genre == expectedGenre);
      CHECK(first.year == 1960);
      CHECK(second.year == 1970);
    };
    auto saveAndWait = [&]
    {
      REQUIRE(save->is_sensitive());
      emitClicked(*save);
      REQUIRE(tryPumpGtkEventsUntil([&dialog] { return !dialog.get_visible(); }));
    };

    SECTION("opening and submitting an untouched form does not copy the first album or the marker")
    {
      CHECK_FALSE(save->is_sensitive());
      CHECK(findButtonByLabel(dialog, clearLabel(MessageId::TrackFieldFilePath)) == nullptr);
      CHECK(findButtonByLabel(dialog, clearLabel(MessageId::TrackFieldConductor)) == nullptr);
      dialog.response(Gtk::ResponseType::OK);
      REQUIRE(tryPumpGtkEventsUntil([&dialog] { return !dialog.get_visible(); }));
      checkTracks("First", "Second", "Jazz");
    }

    SECTION("editing a different field preserves both albums")
    {
      genre->set_text("Classical");
      saveAndWait();
      checkTracks("First", "Second", "Classical");
    }

    SECTION("a mixed album can be replaced without writing before Save")
    {
      REQUIRE(album->is_sensitive());
      REQUIRE(album->get_editable());
      album->set_text("Together");
      checkTracks("First", "Second", "Jazz");
      saveAndWait();
      checkTracks("Together", "Together", "Jazz");
    }

    SECTION("erasing mixed and common replacements restores both baselines")
    {
      album->set_text("Temporary");
      album->set_text("");
      genre->set_text("Temporary");
      genre->set_text("");
      CHECK_FALSE(save->is_sensitive());
      auto* const title = findEntryForLabel(dialog, fieldLabel(MessageId::TrackFieldTitle));
      REQUIRE(title != nullptr);
      title->set_text("Renamed");
      saveAndWait();
      checkTracks("First", "Second", "Jazz");
    }

    SECTION("invalid batch numeric text cannot be clamped and written")
    {
      genre->set_text("Classical");
      auto* const year = findEntryForLabel(dialog, fieldLabel(MessageId::TrackFieldYear));
      REQUIRE(year != nullptr);
      year->set_text("65536");
      genre->grab_focus();
      CHECK_FALSE(save->is_sensitive());
      dialog.response(Gtk::ResponseType::OK);
      drainGtkEvents();
      REQUIRE(dialog.get_visible());
      checkTracks("First", "Second", "Jazz");
      year->set_text("");
      saveAndWait();
      checkTracks("First", "Second", "Classical");
    }

    SECTION("leaving a withdrawn numeric draft displays its baseline instead of zero")
    {
      auto* const year = findEntryForLabel(dialog, fieldLabel(MessageId::TrackFieldYear));
      auto* const disc = findEntryForLabel(dialog, fieldLabel(MessageId::TrackFieldDiscNumber));
      REQUIRE(year != nullptr);
      REQUIRE(disc != nullptr);
      REQUIRE(year->grab_focus());
      year->set_text("0");
      CHECK(year->get_text() == "0");
      year->set_text("02003");
      REQUIRE(disc->grab_focus());
      CHECK(year->get_text() == "02003");
      CHECK(save->is_sensitive());
      disc->set_text("0");
      REQUIRE(year->grab_focus());
      CHECK(disc->get_text().empty());
      CHECK(disc->get_placeholder_text() == "1");
      year->set_text("00");
      REQUIRE(genre->grab_focus());
      CHECK(year->get_text().empty());
      CHECK(year->get_placeholder_text().raw() == fieldLabel(MessageId::TrackMultipleValues));
      CHECK_FALSE(save->is_sensitive());
      genre->set_text("Classical");
      saveAndWait();
      checkTracks("First", "Second", "Classical");
      CHECK(rt::test::runtimeTrackSpec(runtime, ids.front()).discNumber == 1);
      CHECK(rt::test::runtimeTrackSpec(runtime, ids.back()).discNumber == 1);
    }

    SECTION("explicitly clearing a named field on two tracks is a staged action")
    {
      auto const label = clearLabel(MessageId::TrackFieldAlbum);
      auto* const clear = findButtonByLabel(dialog, label);
      REQUIRE(clear != nullptr);
      CHECK(hasAccessibleLabel(*clear, label));
      REQUIRE(clear->is_sensitive());
      REQUIRE(clear->get_visible());
      album->set_text("Draft");
      emitClicked(*clear);
      REQUIRE(save->is_sensitive());
      checkTracks("First", "Second", "Jazz");

      SECTION("cancel keeps both originals")
      {
        dialog.response(Gtk::ResponseType::CANCEL);
        REQUIRE(tryPumpGtkEventsUntil([&dialog] { return !dialog.get_visible(); }));
        checkTracks("First", "Second", "Jazz");
      }

      SECTION("unchecking Clear withdraws the old replacement as well as the clear")
      {
        emitClicked(*clear);
        CHECK(album->get_editable());
        CHECK(album->get_text().empty());
        CHECK_FALSE(save->is_sensitive());
        genre->set_text("Classical");
        saveAndWait();
        checkTracks("First", "Second", "Classical");
      }

      SECTION("save clears both originals")
      {
        saveAndWait();
        checkTracks("", "", "Jazz");
      }
    }
  }

  TEST_CASE("TrackPropertiesDialog - keeps a presented draft until save settlement", "[concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                                     {
                                       trackId = library::test::addTrackWithUniqueFixtureUri(
                                         musicLibrary, {.title = "Before save", .artist = "Settlement Artist"});
                                     }};
    auto& runtime = fixture.runtime();
    auto window = Gtk::Window{};

    REQUIRE(trackId != kInvalidTrackId);

    auto dialog = TrackPropertiesDialog{window,
                                        runtime.async(),
                                        fixture.editing(),
                                        runtime.completion(),
                                        ao::test::englishMessageCatalog(),
                                        std::array{trackId}};
    dialog.present();
    drainGtkEvents();

    SECTION("accepted save keeps the exact pending draft and closes only after readback is committed")
    {
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
      auto* const titleEntry = findEntryByText(dialog, "Before save");
      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(titleEntry != nullptr);
      REQUIRE(saveButton != nullptr);
      REQUIRE(tryPumpGtkEventsUntil([&dialog] { return dialog.get_mapped(); }));
      titleEntry->set_text("Unavailable draft");

      REQUIRE(runGtkTask(
        runtime, fixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Invalidate properties"})));
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

  TEST_CASE("TrackPropertiesDialog - active Credits child blocks parent Save and stages one composed patch")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& storage)
      {
        trackId = library::test::addTrackWithUniqueFixtureUri(
          storage, {.title = "Before", .credits = {{"Player", library::CreditKind::Performer, "Piano"}}});
      }};
    auto& runtime = fixture.runtime();
    auto catalog = ao::test::englishMessageCatalog();
    auto parent = Gtk::Window{};
    parent.present();
    auto* dialog = Gtk::make_managed<TrackPropertiesDialog>(
      parent, runtime.async(), fixture.editing(), runtime.completion(), catalog, std::vector{trackId});
    dialog->present();
    drainGtkEvents();
    auto entries = collectAll<Gtk::Entry>(*dialog);
    auto title = std::ranges::find_if(entries, [](auto* entry) { return entry->get_text() == "Before"; });
    REQUIRE(title != entries.end());
    (*title)->set_text("After");
    auto* edit = findWidgetByClass<Gtk::Button>(*dialog, "ao-credits-edit");
    REQUIRE(edit != nullptr);
    emitClicked(*edit);
    drainGtkEvents();
    auto* name = findWidgetByClass<Gtk::Entry>(*dialog, "ao-credit-name");
    REQUIRE(name != nullptr);
    name->set_text("Changed player");
    auto* save = findButtonByLabel(*dialog, "Save");
    REQUIRE(save != nullptr);
    CHECK_FALSE(save->get_sensitive());
    // Programmatic responses cannot bypass the active-child guard.
    dialog->response(Gtk::ResponseType::OK);
    drainGtkEvents();
    CHECK(rt::test::runtimeTrackSpec(runtime, trackId).title == "Before");
    CHECK(runtime.library().snapshot().trackCredits(trackId) ==
          std::vector<library::Credit>{{"Player", library::CreditKind::Performer, "Piano"}});
    auto* commit = findWidgetByClass<Gtk::Button>(*dialog, "ao-credits-commit");
    REQUIRE(commit != nullptr);
    emitClicked(*commit);
    drainGtkEvents();
    CHECK(save->get_sensitive());
    CHECK(runtime.library().snapshot().trackCredits(trackId) ==
          std::vector<library::Credit>{{"Player", library::CreditKind::Performer, "Piano"}});
    auto finalization = FinalizationObserver{G_OBJECT(dialog->gobj())};
    emitClicked(*save);
    REQUIRE(tryPumpGtkEventsUntil([&] { return finalization.isFinalized(); }));
    CHECK(rt::test::runtimeTrackSpec(runtime, trackId).title == "After");
    CHECK(runtime.library().snapshot().trackCredits(trackId) ==
          std::vector<library::Credit>{{"Changed player", library::CreditKind::Performer, "Piano"}});
    parent.close();
    drainGtkEvents();
  }

  TEST_CASE("TrackPropertiesDialog - closing with a Credits draft asks before discarding it")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto const before = std::vector<library::Credit>{{"Player", library::CreditKind::Performer, "Piano"}};
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& storage)
      { trackId = library::test::addTrackWithUniqueFixtureUri(storage, {.title = "Before", .credits = before}); }};
    auto& runtime = fixture.runtime();
    auto catalog = ao::test::englishMessageCatalog();
    auto parent = Gtk::Window{};
    parent.present();
    auto* dialog = Gtk::make_managed<TrackPropertiesDialog>(
      parent, runtime.async(), fixture.editing(), runtime.completion(), catalog, std::vector{trackId});
    dialog->present();
    drainGtkEvents();
    auto const promptTitle = std::string{"Discard the Credits changes?"};
    auto awaitPrompt = [&]
    {
      AppDialog* prompt = nullptr;
      REQUIRE(tryPumpGtkEventsUntil(
        [&]
        {
          prompt = findOtherDialogWithText(*dialog, promptTitle);
          return prompt != nullptr;
        }));
      return prompt;
    };

    auto* edit = findWidgetByClass<Gtk::Button>(*dialog, "ao-credits-edit");
    REQUIRE(edit != nullptr);
    emitClicked(*edit);
    drainGtkEvents();
    auto* name = findWidgetByClass<Gtk::Entry>(*dialog, "ao-credit-name");
    REQUIRE(name != nullptr);
    name->set_text("Draft player");

    SECTION("an active child draft survives Keep Editing")
    {
      dialog->response(Gtk::ResponseType::CLOSE);
      auto* prompt = awaitPrompt();
      CHECK(dialog->get_visible());
      prompt->response(Gtk::ResponseType::CANCEL);
      drainGtkEvents();
      REQUIRE(std::ranges::contains(Gtk::Window::list_toplevels(), dialog));
      CHECK(dialog->get_visible());
      CHECK(findOtherDialogWithText(*dialog, promptTitle) == nullptr);
      name = findWidgetByClass<Gtk::Entry>(*dialog, "ao-credit-name");
      REQUIRE(name != nullptr);
      CHECK(name->get_text() == "Draft player");

      // A repeated close request returns to one live question.
      dialog->close();
      prompt = awaitPrompt();
      dialog->close();
      drainGtkEvents();
      auto prompts = std::ranges::count_if(
        Gtk::Window::list_toplevels(),
        [&](Gtk::Window* window) { return window != dialog && findLabelByText(*window, promptTitle) != nullptr; });
      CHECK(prompts == 1);
      prompt->response(Gtk::ResponseType::CANCEL);
      drainGtkEvents();
      CHECK(dialog->get_visible());

      dialog->response(Gtk::ResponseType::CLOSE);
      prompt = awaitPrompt();
      auto finalization = FinalizationObserver{G_OBJECT(dialog->gobj())};
      prompt->response(Gtk::ResponseType::REJECT);
      REQUIRE(tryPumpGtkEventsUntil([&] { return finalization.isFinalized(); }));
    }

    SECTION("an accepted but unsaved replacement is discarded only on confirmation")
    {
      auto* commit = findWidgetByClass<Gtk::Button>(*dialog, "ao-credits-commit");
      REQUIRE(commit != nullptr);
      emitClicked(*commit);
      drainGtkEvents();
      dialog->close();
      auto* prompt = awaitPrompt();
      auto finalization = FinalizationObserver{G_OBJECT(dialog->gobj())};
      prompt->response(Gtk::ResponseType::REJECT);
      REQUIRE(tryPumpGtkEventsUntil([&] { return finalization.isFinalized(); }));
    }

    CHECK(runtime.library().snapshot().trackCredits(trackId) == before);
    CHECK(rt::test::runtimeTrackSpec(runtime, trackId).title == "Before");
    parent.close();
    drainGtkEvents();
  }

  TEST_CASE("TrackPropertiesDialog - closing an unchanged Credits session does not ask")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto const before = std::vector<library::Credit>{{"Player", library::CreditKind::Performer, "Piano"}};
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& storage)
      { trackId = library::test::addTrackWithUniqueFixtureUri(storage, {.title = "Before", .credits = before}); }};
    auto& runtime = fixture.runtime();
    auto catalog = ao::test::englishMessageCatalog();
    auto parent = Gtk::Window{};
    parent.present();
    auto* dialog = Gtk::make_managed<TrackPropertiesDialog>(
      parent, runtime.async(), fixture.editing(), runtime.completion(), catalog, std::vector{trackId});
    dialog->present();
    drainGtkEvents();
    auto* edit = findWidgetByClass<Gtk::Button>(*dialog, "ao-credits-edit");
    REQUIRE(edit != nullptr);
    emitClicked(*edit);
    drainGtkEvents();
    auto* name = findWidgetByClass<Gtk::Entry>(*dialog, "ao-credit-name");
    REQUIRE(name != nullptr);

    // An open session, even one whose edit was typed back to the baseline,
    // holds nothing a close would lose.
    name->set_text("Draft player");
    name->set_text("Player");
    auto finalization = FinalizationObserver{G_OBJECT(dialog->gobj())};
    dialog->close();
    REQUIRE(tryPumpGtkEventsUntil([&] { return finalization.isFinalized(); }));
    CHECK(std::ranges::none_of(Gtk::Window::list_toplevels(),
                               [](Gtk::Window* window)
                               { return findLabelByText(*window, "Discard the Credits changes?") != nullptr; }));
    CHECK(runtime.library().snapshot().trackCredits(trackId) == before);
    parent.close();
    drainGtkEvents();
  }
} // namespace ao::gtk::test
