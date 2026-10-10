// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/linux-gtk/layout/component/track/TrackCreditsEditor.h"

#include "app/linux-gtk/layout/component/track/TrackDetailUndo.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkLayoutTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <gtkmm/button.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/root.h>
#include <gtkmm/scrolledwindow.h>
#include <sigc++/functors/slot.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace ao::gtk::test;
  namespace
  {
    using library::Credit;
    using library::CreditKind;

    // Both production entry points place the growing list in a scrolling
    // viewport. Keep native configure replies for old row counts from becoming
    // a toplevel minimum-size violation while the next resize is pending.
    class CreditsEditorWindowFixture final
    {
    public:
      CreditsEditorWindowFixture()
      {
        _scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        _scroll.set_propagate_natural_height(true);
        _window.mount(_scroll);
      }

      ~CreditsEditorWindowFixture() { unmount(); }

      CreditsEditorWindowFixture(CreditsEditorWindowFixture const&) = delete;
      CreditsEditorWindowFixture& operator=(CreditsEditorWindowFixture const&) = delete;
      CreditsEditorWindowFixture(CreditsEditorWindowFixture&&) = delete;
      CreditsEditorWindowFixture& operator=(CreditsEditorWindowFixture&&) = delete;

      void mount(Gtk::Widget& editor) { _scroll.set_child(editor); }
      void present() { _window.present(); }
      void unmount()
      {
        _scroll.unset_child();
        _window.unmount();
      }
      void detachWithoutDraining()
      {
        _scroll.unset_child();
        _window.detachWithoutDraining();
      }
      void drain() { _window.drain(); }

    private:
      GtkWindowFixture _window;
      Gtk::ScrolledWindow _scroll;
    };

    Gtk::Button& button(Gtk::Widget& root, std::string const& cssClass)
    {
      auto* found = findWidgetByClass<Gtk::Button>(root, cssClass);
      REQUIRE(found != nullptr);
      return *found;
    }

    Gtk::Entry& entry(Gtk::Widget& root, std::string const& cssClass)
    {
      auto* found = findWidgetByClass<Gtk::Entry>(root, cssClass);
      REQUIRE(found != nullptr);
      return *found;
    }

    void click(Gtk::Widget& root, std::string const& cssClass)
    {
      emitClicked(button(root, cssClass));
      drainGtkEvents();
    }

    void checkFocusWithin(Gtk::Widget& widget)
    {
      auto* root = widget.get_root();
      REQUIRE(root != nullptr);
      auto* focus = root->get_focus();
      REQUIRE(focus != nullptr);
      CHECK((focus == &widget || focus->is_ancestor(widget)));
    }

    std::vector<Credit> credits(rt::AppRuntime& runtime, TrackId id)
    {
      auto snapshot = runtime.library().snapshot();
      auto optEntries = snapshot.trackCredits(id);
      REQUIRE(optEntries);
      return *optEntries;
    }

    rt::TrackDetailSnapshot detail(rt::AppRuntime& runtime, std::vector<TrackId> ids)
    {
      auto snapshot = runtime.library().snapshot();
      auto sections = ao::test::requireValue(uimodel::loadTrackCreditsEditorBaseline(snapshot, ids));
      return {.trackIds = std::move(ids), .credits = std::move(sections)};
    }

    rt::CreditReplacement replacement(std::vector<Credit> entries)
    {
      return {.kinds = uimodel::allTrackCreditKinds(), .entries = std::move(entries)};
    }
  } // namespace

  TEST_CASE("TrackCreditsEditor - clear undo reads its bound snapshot rather than stale display",
            "[gtk][integration][track-credits]")
  {
    auto id = kInvalidTrackId;
    auto const before =
      std::vector<Credit>{{"Current", CreditKind::Performer, "Piano"}, {"Current", CreditKind::Performer, "Voice"}};
    auto fixture =
      LayoutRuntimeFixture{"io.github.aobus.credits_clear_test",
                           [&](library::MusicLibrary& storage)
                           { id = library::test::addTrackWithUniqueFixtureUri(storage, {.credits = before}); }};
    auto& runtime = fixture.runtime();
    // A stale empty projection must not determine the Undo pre-value.
    auto& scope = fixture.attachTrackDetailScope({.trackIds = {id}});
    auto undo = TrackDetailUndoController{};
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.async(),
                                     runtime.library(),
                                     runtime.completion(),
                                     runtime.notifications(),
                                     ao::test::englishMessageCatalog(),
                                     &scope,
                                     &undo};
    windowFixture.mount(editor);
    windowFixture.present();
    editor.updateVisibility(true, true);
    click(editor, "ao-credits-edit");
    click(editor, "ao-credits-clear");
    click(editor, "ao-credits-commit");
    REQUIRE(tryPumpGtkEventsUntil([&] { return undo.pendingCreditsUndo() != nullptr; }));
    CHECK(credits(runtime, id).empty());
    CHECK(undo.pendingCreditsUndo()->replacement.entries == before);
    CHECK(undo.pendingCreditsUndo()->replacement.kinds.all());
    CHECK(undo.pendingCreditsUndo()->session.isCurrent());
    REQUIRE(runGtkTask(runtime, undo.undoAsync()));
    CHECK(credits(runtime, id) == before);
    CHECK_FALSE(undo.pendingCreditsUndo());
    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor - mixed scope requires explicit intent and rejects role-only rows",
            "[gtk][integration][track-credits]")
  {
    auto first = kInvalidTrackId;
    auto second = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.credits_mixed_test",
                                        [&](library::MusicLibrary& storage)
                                        {
                                          first = library::test::addTrackWithUniqueFixtureUri(
                                            storage, {.credits = {{"A", CreditKind::Performer, "Piano"}}});
                                          second = library::test::addTrackWithUniqueFixtureUri(
                                            storage, {.credits = {{"B", CreditKind::Performer, "Violin"}}});
                                        }};
    auto& runtime = fixture.runtime();
    auto& scope = fixture.attachTrackDetailScope(detail(runtime, {first, second}));
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.async(),
                                     runtime.library(),
                                     runtime.completion(),
                                     runtime.notifications(),
                                     ao::test::englishMessageCatalog(),
                                     &scope,
                                     nullptr};
    windowFixture.mount(editor);
    windowFixture.present();
    click(editor, "ao-credits-edit");
    CHECK(collectAll<Gtk::Entry>(editor).empty());
    CHECK_FALSE(button(editor, "ao-credits-commit").get_sensitive());
    CHECK(button(editor, "ao-credits-replace").get_visible());
    checkFocusWithin(button(editor, "ao-credits-replace"));
    click(editor, "ao-credits-replace");
    checkFocusWithin(button(editor, "ao-credits-add"));
    click(editor, "ao-credits-add");
    checkFocusWithin(entry(editor, "ao-credit-name"));
    entry(editor, "ao-credit-role").set_text("Piano");
    CHECK_FALSE(button(editor, "ao-credits-commit").get_sensitive());
    CHECK(entry(editor, "ao-credit-name").has_css_class("error"));
    click(editor, "ao-credits-commit");
    CHECK(credits(runtime, first) == std::vector<Credit>{{"A", CreditKind::Performer, "Piano"}});
    CHECK(credits(runtime, second) == std::vector<Credit>{{"B", CreditKind::Performer, "Violin"}});
    click(editor, "ao-credits-cancel");
    CHECK(collectAll<Gtk::Entry>(editor).empty());
    checkFocusWithin(button(editor, "ao-credits-edit"));
    click(editor, "ao-credits-edit");
    click(editor, "ao-credits-replace");
    click(editor, "ao-credits-add");
    scope.setSnapshot(detail(runtime, {second}));
    drainGtkEvents();
    CHECK(collectAll<Gtk::Entry>(editor).empty());
    CHECK(findLabelByText(editor, "B") != nullptr);
    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor - selection replacement cannot retarget an admitted edit",
            "[gtk][integration][track-credits][concurrency]")
  {
    auto first = kInvalidTrackId;
    auto second = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.credits_selection_test",
                                        [&](library::MusicLibrary& storage)
                                        {
                                          first = library::test::addTrackWithUniqueFixtureUri(
                                            storage, {.credits = {{"A", CreditKind::Performer, "Piano"}}});
                                          second = library::test::addTrackWithUniqueFixtureUri(
                                            storage, {.credits = {{"B", CreditKind::Performer, "Violin"}}});
                                        }};
    auto& runtime = fixture.runtime();
    auto& scope = fixture.attachTrackDetailScope(detail(runtime, {first}));
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.async(),
                                     runtime.library(),
                                     runtime.completion(),
                                     runtime.notifications(),
                                     ao::test::englishMessageCatalog(),
                                     &scope,
                                     nullptr};
    windowFixture.mount(editor);
    windowFixture.present();
    click(editor, "ao-credits-edit");
    entry(editor, "ao-credit-name").set_text("Replacement");
    emitClicked(button(editor, "ao-credits-commit"));
    scope.setSnapshot(detail(runtime, {second}));
    REQUIRE(tryPumpGtkEventsUntil([&] { return credits(runtime, first)[0].name == "Replacement"; }));
    drainGtkEvents();
    CHECK(credits(runtime, second) == std::vector<Credit>{{"B", CreditKind::Performer, "Violin"}});
    CHECK(collectAll<Gtk::Entry>(editor).empty());
    CHECK(findLabelByText(editor, "B") != nullptr);
    CHECK(findLabelByText(editor, "Replacement") == nullptr);
    windowFixture.unmount();
  }

  TEST_CASE("TrackDetailUndoController - retired credits replay and timer cannot clear a replacement",
            "[gtk][integration][track-credits][concurrency]")
  {
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.credits_undo_generation_test",
                                        [&](library::MusicLibrary& storage)
                                        { id = library::test::addTrackWithUniqueFixtureUri(storage, {}); }};
    auto& runtime = fixture.runtime();
    auto timeouts = std::vector<sigc::slot<bool()>>{};
    auto undo = TrackDetailUndoController{[&](std::chrono::milliseconds, sigc::slot<bool()> callback)
                                          {
                                            timeouts.push_back(std::move(callback));
                                            return sigc::connection{};
                                          }};
    auto session = ao::test::requireValue(uimodel::TrackAuthoringSession::begin(runtime.library(), std::array{id}));
    auto const publishedRevision = session.boundRevision() + 1;
    undo.presentCreditsClearedUndo(replacement({{"Restored", CreditKind::Performer, "Piano"}}), std::move(session));
    bool replayReturned = false;
    bool replacedBeforeReplayReturned = false;
    [[maybe_unused]] auto subscription = runtime.library().onAuthoringAvailabilityChanged(
      [&](rt::LibraryAuthoringAvailability const& availability) noexcept
      {
        if (availability.libraryRevision != publishedRevision)
        {
          return;
        }

        auto replacementSessionRes = uimodel::TrackAuthoringSession::begin(runtime.library(), std::array{id});

        if (replacementSessionRes)
        {
          undo.presentCustomMetadataDeletedUndo("Mood", "Bright", std::move(*replacementSessionRes));
          replacedBeforeReplayReturned = !replayReturned && undo.pendingCustomMetadataUndo() != nullptr;
        }
      });
    auto const replayRes = runGtkTask(runtime, undo.undoAsync());
    replayReturned = true;
    REQUIRE(replayRes);
    CHECK(replacedBeforeReplayReturned);
    REQUIRE(undo.pendingCustomMetadataUndo());
    CHECK(undo.pendingCustomMetadataUndo()->key == "Mood");
    CHECK(credits(runtime, id) == std::vector<Credit>{{"Restored", CreditKind::Performer, "Piano"}});
    REQUIRE(timeouts.size() == 2);
    CHECK_FALSE(timeouts[0]());
    CHECK(undo.pendingCustomMetadataUndo() != nullptr);
    CHECK_FALSE(timeouts[1]());
    CHECK_FALSE(undo.pendingCustomMetadataUndo());
  }

  TEST_CASE("TrackDetailUndoController - only overlapping scopes and newer mutations retire credits undo",
            "[gtk][unit][track-credits]")
  {
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.credits_undo_overlap_test",
                                        [&](library::MusicLibrary& storage)
                                        { id = library::test::addTrackWithUniqueFixtureUri(storage, {}); }};
    auto& runtime = fixture.runtime();
    auto undo = TrackDetailUndoController{};
    auto session = ao::test::requireValue(uimodel::TrackAuthoringSession::begin(runtime.library(), std::array{id}));
    auto const revision = session.boundRevision();
    auto const kinds = uimodel::trackCreditScope(CreditKind::Conductor);
    REQUIRE(revision > 0);
    undo.presentCreditsClearedUndo({.kinds = kinds, .entries = {{"A", CreditKind::Conductor, ""}}}, std::move(session));
    undo.clearIfAffectsCredits({id}, revision - 1, kinds);
    CHECK(undo.pendingCreditsUndo() != nullptr);
    undo.clearIfAffectsCredits({TrackId{999999}}, revision, kinds);
    CHECK(undo.pendingCreditsUndo() != nullptr);
    undo.clearIfAffectsCredits({id}, revision, uimodel::trackCreditScope(CreditKind::Performer));
    CHECK(undo.pendingCreditsUndo() != nullptr);
    undo.clearIfAffectsCredits({id}, revision, kinds);
    CHECK_FALSE(undo.pendingCreditsUndo());
  }

  TEST_CASE("TrackCreditsEditor - retired rows ignore late input before their queued replacement",
            "[gtk][integration][track-credits][async]")
  {
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.credits_row_retirement_test",
      [&](library::MusicLibrary& storage)
      {
        id = library::test::addTrackWithUniqueFixtureUri(
          storage, {.credits = {{"A", CreditKind::Performer, "Piano"}, {"B", CreditKind::Performer, "Violin"}}});
      }};
    auto& runtime = fixture.runtime();
    auto& scope = fixture.attachTrackDetailScope(detail(runtime, {id}));
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.async(),
                                     runtime.library(),
                                     runtime.completion(),
                                     runtime.notifications(),
                                     ao::test::englishMessageCatalog(),
                                     &scope,
                                     nullptr};
    windowFixture.mount(editor);
    windowFixture.present();
    click(editor, "ao-credits-edit");
    auto& retiredName = entry(editor, "ao-credit-name");
    REQUIRE(retiredName.get_text() == "A");
    REQUIRE(retiredName.grab_focus());
    emitClicked(button(editor, "ao-credit-delete"));
    auto* focus = editor.get_root()->get_focus();
    CHECK((focus == nullptr || !focus->is_ancestor(retiredName)));
    retiredName.set_text("Late A");
    drainGtkEvents();
    auto& currentName = entry(editor, "ao-credit-name");
    CHECK(currentName.get_text() == "B");
    checkFocusWithin(currentName);
    emitClicked(button(editor, "ao-credits-commit"));
    focus = editor.get_root()->get_focus();
    CHECK((focus == nullptr || !focus->is_ancestor(currentName)));
    REQUIRE(tryPumpGtkEventsUntil([&] { return collectAll<Gtk::Entry>(editor).empty(); }));
    CHECK(credits(runtime, id) == std::vector<Credit>{{"B", CreditKind::Performer, "Violin"}});
    CHECK(findLabelByText(editor, "Late A") == nullptr);
    checkFocusWithin(button(editor, "ao-credits-edit"));
    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor - queued rows retire safely with their owner", "[gtk][unit][track-credits][async]")
  {
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.credits_row_teardown_test",
                                        [&](library::MusicLibrary& storage)
                                        { id = library::test::addTrackWithUniqueFixtureUri(storage, {}); }};
    auto& runtime = fixture.runtime();
    auto& scope = fixture.attachTrackDetailScope({.trackIds = {id}});
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editorPtr = std::make_unique<TrackCreditsEditor>(runtime.async(),
                                                          runtime.library(),
                                                          runtime.completion(),
                                                          runtime.notifications(),
                                                          ao::test::englishMessageCatalog(),
                                                          &scope,
                                                          nullptr);
    windowFixture.mount(*editorPtr);
    windowFixture.present();
    editorPtr->updateVisibility(true, true);
    click(*editorPtr, "ao-credits-edit");
    click(*editorPtr, "ao-credits-add");
    entry(*editorPtr, "ao-credit-name").set_text("Uncommitted");
    REQUIRE(collectAll<Gtk::Entry>(*editorPtr).size() == 2);
    emitClicked(button(*editorPtr, "ao-credits-add"));
    REQUIRE(collectAll<Gtk::Entry>(*editorPtr).size() == 2);
    windowFixture.detachWithoutDraining();
    editorPtr.reset();
    windowFixture.drain();
    CHECK(credits(runtime, id).empty());
  }

  TEST_CASE("TrackCreditsEditor - Properties category and full editors stage attribute-preserving controls",
            "[gtk][unit][track-credits]")
  {
    auto id = kInvalidTrackId;
    auto const before = std::vector<Credit>{{"A", CreditKind::Conductor, "Lead"},
                                            {"B", CreditKind::Conductor, "Guest"},
                                            {"C", CreditKind::Performer, "Piano"}};
    auto fixture =
      LayoutRuntimeFixture{"io.github.aobus.credits_properties_test",
                           [&](library::MusicLibrary& storage)
                           { id = library::test::addTrackWithUniqueFixtureUri(storage, {.credits = before}); }};
    auto& runtime = fixture.runtime();
    auto const& catalog = ao::test::englishMessageCatalog();
    auto form = uimodel::TrackPropertiesFormModel{catalog};
    REQUIRE(uimodel::loadTrackPropertiesFormBaseline(
      runtime.library().snapshot(), std::array{id}, uimodel::buildTrackPropertiesFormSpec(catalog), form));
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.completion(), catalog, form, [] {}};
    windowFixture.mount(editor);
    windowFixture.present();
    click(editor, "ao-credit-category");
    checkFocusWithin(entry(editor, "ao-credit-name"));
    CHECK_FALSE(form.canSave());
    auto* kind = findWidgetByClass<Gtk::ComboBoxText>(editor, "ao-credit-kind");
    REQUIRE(kind != nullptr);
    CHECK_FALSE(kind->get_visible());
    entry(editor, "ao-credit-name").set_text("Changed");
    click(editor, "ao-credit-down");
    auto entries = collectAll<Gtk::Entry>(editor);
    REQUIRE(entries.size() == 4);
    auto moved = std::ranges::find_if(entries, [](auto* value) { return value->get_text() == "Changed"; });
    REQUIRE(moved != entries.end());
    checkFocusWithin(**moved);
    click(editor, "ao-credits-commit");
    checkFocusWithin(button(editor, "ao-credit-category"));
    REQUIRE(form.pendingCredits());
    CHECK(form.pendingCredits()->kinds == uimodel::trackCreditScope(CreditKind::Conductor));
    CHECK(form.pendingCredits()->entries ==
          std::vector<Credit>{{"B", CreditKind::Conductor, "Guest"}, {"Changed", CreditKind::Conductor, "Lead"}});
    CHECK(credits(runtime, id) == before);
    click(editor, "ao-credits-edit");
    kind = findWidgetByClass<Gtk::ComboBoxText>(editor, "ao-credit-kind");
    REQUIRE(kind != nullptr);
    CHECK(kind->get_visible());
    kind->set_active(static_cast<std::int32_t>(CreditKind::Performer));
    drainGtkEvents();
    click(editor, "ao-credits-commit");
    CHECK(form.pendingCredits()->kinds.all());
    CHECK(form.pendingCredits()->entries == std::vector<Credit>{{"Changed", CreditKind::Conductor, "Lead"},
                                                                {"C", CreditKind::Performer, "Piano"},
                                                                {"B", CreditKind::Performer, "Guest"}});
    CHECK(credits(runtime, id) == before);
    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor - empty scopes focus Add and restore the originating action",
            "[gtk][unit][track-credits]")
  {
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.credits_empty_focus_test",
                                        [&](library::MusicLibrary& storage)
                                        { id = library::test::addTrackWithUniqueFixtureUri(storage, {}); }};
    auto& runtime = fixture.runtime();
    auto const& catalog = ao::test::englishMessageCatalog();
    auto form = uimodel::TrackPropertiesFormModel{catalog};
    REQUIRE(uimodel::loadTrackPropertiesFormBaseline(
      runtime.library().snapshot(), std::array{id}, uimodel::buildTrackPropertiesFormSpec(catalog), form));
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.completion(), catalog, form, [] {}};
    windowFixture.mount(editor);
    windowFixture.present();
    auto origins = std::vector{&button(editor, "ao-credits-edit")};

    for (auto* action : collectAll<Gtk::Button>(editor))
    {
      if (action->has_css_class("ao-credit-category"))
      {
        origins.push_back(action);
      }
    }

    REQUIRE(origins.size() == library::kCreditKindCount + 1);

    for (auto* origin : origins)
    {
      CAPTURE(origin->get_label());
      emitClicked(*origin);
      drainGtkEvents();
      checkFocusWithin(button(editor, "ao-credits-add"));
      click(editor, "ao-credits-add");
      checkFocusWithin(entry(editor, "ao-credit-name"));
      click(editor, "ao-credit-delete");
      checkFocusWithin(button(editor, "ao-credits-add"));
      click(editor, "ao-credits-cancel");
      checkFocusWithin(*origin);
      CHECK_FALSE(form.pendingCredits());
    }

    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor - validation identifies a later row in the active locale", "[gtk][unit][track-credits]")
  {
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.credits_localized_error_test",
      [&](library::MusicLibrary& storage)
      {
        id = library::test::addTrackWithUniqueFixtureUri(
          storage,
          {.credits = {{"First", CreditKind::Performer, "Piano"}, {"Second", CreditKind::Performer, "Voice"}}});
      }};
    auto& runtime = fixture.runtime();
    auto form = uimodel::TrackPropertiesFormModel{ao::test::messageCatalog("zh-CN")};
    REQUIRE(
      uimodel::loadTrackPropertiesFormBaseline(runtime.library().snapshot(),
                                               std::array{id},
                                               uimodel::buildTrackPropertiesFormSpec(ao::test::messageCatalog("zh-CN")),
                                               form));
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.completion(), ao::test::messageCatalog("zh-CN"), form, [] {}};
    windowFixture.mount(editor);
    windowFixture.present();
    click(editor, "ao-credits-edit");
    auto names = std::vector<Gtk::Entry*>{};

    for (auto* field : collectAll<Gtk::Entry>(editor))
    {
      if (field->has_css_class("ao-credit-name"))
      {
        names.push_back(field);
      }
    }

    REQUIRE(names.size() == 2);
    names[1]->set_text("   ");
    CHECK_FALSE(names[0]->has_css_class("error"));
    CHECK(names[1]->has_css_class("error"));
    CHECK_FALSE(button(editor, "ao-credits-commit").get_sensitive());
    auto const text = uimodel::formatTrackCreditValidationError(
      ao::test::messageCatalog("zh-CN"), {.rowIndex = 1, .reason = uimodel::TrackCreditValidationReason::BlankName});
    CHECK(findLabelByText(editor, text) != nullptr);
    emitClicked(button(editor, "ao-credits-commit"));
    checkFocusWithin(*names[1]);
    CHECK(form.creditsEditor().entries()[1].name == "   ");
    CHECK_FALSE(form.pendingCredits());
    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor - Busy retains the complete draft while publication settles",
            "[gtk][integration][track-credits][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto temp = ao::test::TempDir{};
    auto executorPtr = std::make_unique<rt::test::ManualExecutor>();
    auto* executor = executorPtr.get();
    auto runtimePtr = rt::test::makeRuntime(temp, std::move(executorPtr));
    auto& runtime = *runtimePtr;
    auto retireWork = utility::ScopedRegistration{[&runtime, executor]
                                                  {
                                                    runtime.shutdown();
                                                    executor->runUntilIdle();
                                                  }};
    auto const before =
      std::vector<Credit>{{"First", CreditKind::Performer, "Piano"}, {"Second", CreditKind::Performer, "Voice"}};
    auto const id = rt::test::addRuntimeTrack(runtime, {.credits = before});
    auto scope = FakeTrackDetailScope{detail(runtime, {id})};
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.async(),
                                     runtime.library(),
                                     runtime.completion(),
                                     runtime.notifications(),
                                     ao::test::englishMessageCatalog(),
                                     &scope,
                                     nullptr};
    windowFixture.mount(editor);
    windowFixture.present();
    click(editor, "ao-credits-edit");
    entry(editor, "ao-credit-name").set_text("  Draft  ");
    entry(editor, "ao-credit-role").set_text("  Literal role  ");
    executor->runUntilIdle();

    // Queue the UI workflow entry first, then hold the competing write at its
    // publication. Entering the workflow starts the lazy Credits task; keep
    // publication queued until that task has returned Busy to the executor.
    emitClicked(button(editor, "ao-credits-commit"));
    REQUIRE(executor->tryWaitUntilQueued());
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = runtime.async().spawn(rt::test::flagCompletionAsync(
      completedPtr, runtime.library().commands().createListAsync(rt::ListDraft{.name = "Competing change"})));
    REQUIRE(executor->tryWaitUntilQueuedCount(2));
    REQUIRE(executor->tryRunOne());
    REQUIRE(executor->tryWaitUntilQueuedCount(2));
    auto hasBusy = [&runtime]
    {
      return std::ranges::any_of(runtime.notifications().feed().entries,
                                 [](auto const& notification)
                                 {
                                   auto const* text = std::get_if<std::string>(&notification.message);
                                   return notification.severity == rt::NotificationSeverity::Warning &&
                                          text != nullptr && *text == "Library is busy. Try again.";
                                 });
    };
    REQUIRE(executor->tryDrainUntil([&] { return completedPtr->load() && hasBusy(); }));
    REQUIRE(future.get());
    drainGtkEvents();
    CHECK(credits(runtime, id) == before);
    CHECK(entry(editor, "ao-credit-name").get_text() == "  Draft  ");
    CHECK(entry(editor, "ao-credit-role").get_text() == "  Literal role  ");
    CHECK(collectAll<Gtk::Entry>(editor).size() == 4);
    CHECK(button(editor, "ao-credits-cancel").get_sensitive());
    // The unrelated committed revision stales this binding, not its draft.
    CHECK_FALSE(button(editor, "ao-credits-commit").get_sensitive());
    CHECK(findLabelByText(editor, "Library changed while this edit was open. Reload the value and try again.") !=
          nullptr);
    click(editor, "ao-credits-cancel");
    checkFocusWithin(button(editor, "ao-credits-edit"));
    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor - common category editing preserves each target's mixed other kinds",
            "[gtk][integration][track-credits]")
  {
    auto first = kInvalidTrackId;
    auto second = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.credits_scoped_test",
      [&](library::MusicLibrary& storage)
      {
        first = library::test::addTrackWithUniqueFixtureUri(
          storage, {.credits = {{"Common", CreditKind::Conductor, "Lead"}, {"A", CreditKind::Performer, "Piano"}}});
        second = library::test::addTrackWithUniqueFixtureUri(
          storage, {.credits = {{"Common", CreditKind::Conductor, "Lead"}, {"B", CreditKind::Performer, "Violin"}}});
      }};
    auto& runtime = fixture.runtime();
    auto& scope = fixture.attachTrackDetailScope(detail(runtime, {first, second}));
    auto undo = TrackDetailUndoController{};
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.async(),
                                     runtime.library(),
                                     runtime.completion(),
                                     runtime.notifications(),
                                     ao::test::englishMessageCatalog(),
                                     &scope,
                                     &undo};
    windowFixture.mount(editor);
    windowFixture.present();
    click(editor, "ao-credit-category");
    CHECK_FALSE(button(editor, "ao-credits-replace").get_visible());
    CHECK(entry(editor, "ao-credit-name").get_text() == "Common");
    entry(editor, "ao-credit-name").set_text("Renamed");
    click(editor, "ao-credits-commit");
    REQUIRE(tryPumpGtkEventsUntil([&] { return collectAll<Gtk::Entry>(editor).empty(); }));
    CHECK(credits(runtime, first) ==
          std::vector<Credit>{{"Renamed", CreditKind::Conductor, "Lead"}, {"A", CreditKind::Performer, "Piano"}});
    CHECK(credits(runtime, second) ==
          std::vector<Credit>{{"Renamed", CreditKind::Conductor, "Lead"}, {"B", CreditKind::Performer, "Violin"}});
    click(editor, "ao-credit-category");
    click(editor, "ao-credits-clear");
    click(editor, "ao-credits-commit");
    REQUIRE(tryPumpGtkEventsUntil([&] { return undo.pendingCreditsUndo() != nullptr; }));
    CHECK(undo.pendingCreditsUndo()->replacement.kinds == uimodel::trackCreditScope(CreditKind::Conductor));
    CHECK(credits(runtime, first) == std::vector<Credit>{{"A", CreditKind::Performer, "Piano"}});
    CHECK(credits(runtime, second) == std::vector<Credit>{{"B", CreditKind::Performer, "Violin"}});
    REQUIRE(runGtkTask(runtime, undo.undoAsync()));
    CHECK(credits(runtime, first) ==
          std::vector<Credit>{{"Renamed", CreditKind::Conductor, "Lead"}, {"A", CreditKind::Performer, "Piano"}});
    CHECK(credits(runtime, second) ==
          std::vector<Credit>{{"Renamed", CreditKind::Conductor, "Lead"}, {"B", CreditKind::Performer, "Violin"}});
    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor - wrapped labels keep minimum-height requests consistent across locales",
            "[gtk][unit][track-credits][geometry]")
  {
    auto const* const locale = GENERATE("en-US", "de-DE", "es-ES", "fr-FR", "ja-JP", "zh-CN", "zh-TW");
    CAPTURE(locale);
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.credits_measure_test",
      [&](library::MusicLibrary& storage)
      {
        id = library::test::addTrackWithUniqueFixtureUri(
          storage, {.credits = {{std::string(180, 'N'), CreditKind::Performer, std::string(180, 'R')}}});
      }};
    auto& runtime = fixture.runtime();
    auto& scope = fixture.attachTrackDetailScope(detail(runtime, {id}));
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.async(),
                                     runtime.library(),
                                     runtime.completion(),
                                     runtime.notifications(),
                                     ao::test::messageCatalog(locale),
                                     &scope,
                                     nullptr};
    windowFixture.mount(editor);
    windowFixture.present();

    auto checkMeasurements = [&]
    {
      auto const width = measureWidget(editor, Gtk::Orientation::HORIZONTAL);
      auto const minimumWidthHeight = measureWidget(editor, Gtk::Orientation::VERTICAL, width.minimum);
      CHECK(width.minimum == 0);

      for (std::int32_t const constrainedWidth : {0, 96, 240, 640})
      {
        CAPTURE(constrainedWidth);
        auto const constrainedHeight = measureWidget(editor, Gtk::Orientation::VERTICAL, constrainedWidth);
        CHECK(constrainedHeight.minimum <= minimumWidthHeight.minimum);
        // GTK's inverse minimum-size probe must not discover a taller minimum
        // after measuring the width needed to fit this height.
        auto const inverseWidth = measureWidget(editor, Gtk::Orientation::HORIZONTAL, constrainedHeight.minimum);
        auto const inverseHeight = measureWidget(editor, Gtk::Orientation::VERTICAL, inverseWidth.minimum);
        CHECK(inverseHeight.minimum <= constrainedHeight.minimum);
        editor.size_allocate({0, 0, constrainedWidth, constrainedHeight.minimum}, -1);
        CHECK(editor.get_width() == constrainedWidth);
      }
    };

    checkMeasurements();
    click(editor, "ao-credits-edit");
    checkMeasurements();
    entry(editor, "ao-credit-name").set_text("");
    CHECK_FALSE(button(editor, "ao-credits-commit").get_sensitive());
    checkMeasurements();
    click(editor, "ao-credits-cancel");
    click(editor, "ao-credit-category");
    checkMeasurements();
    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor - localized controls do not impose a wide horizontal minimum",
            "[gtk][unit][track-credits][geometry]")
  {
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.credits_width_test",
      [&](library::MusicLibrary& storage)
      {
        id = library::test::addTrackWithUniqueFixtureUri(
          storage, {.credits = {{std::string(180, 'N'), CreditKind::Performer, std::string(180, 'R')}}});
      }};
    auto& runtime = fixture.runtime();
    auto& scope = fixture.attachTrackDetailScope(detail(runtime, {id}));
    auto windowFixture = CreditsEditorWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.async(),
                                     runtime.library(),
                                     runtime.completion(),
                                     runtime.notifications(),
                                     ao::test::englishMessageCatalog(),
                                     &scope,
                                     nullptr};
    windowFixture.mount(editor);
    windowFixture.present();
    click(editor, "ao-credits-edit");
    std::int32_t minimum = 0;
    std::int32_t natural = 0;
    std::int32_t minimumBaseline = 0;
    std::int32_t naturalBaseline = 0;
    editor.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
    CHECK(minimum == 0);
    CHECK(hasAccessibleLabel(entry(editor, "ao-credit-name"), "Name"));
    CHECK(hasAccessibleLabel(button(editor, "ao-credit-up"), "Move up"));
    CHECK(hasAccessibleLabel(button(editor, "ao-credit-down"), "Move down"));
    windowFixture.unmount();
  }
} // namespace ao::gtk::layout::test
