// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/linux-gtk/layout/component/track/TrackCreditsEditor.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/popover.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace ao::gtk::test;
  using library::Credit;
  using library::CreditKind;

  TEST_CASE("TrackCreditsEditor refresh - queued same-selection refresh preserves live input and completion",
            "[gtk][integration][track-credits][concurrency]")
  {
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.credits_refresh_test",
                                        [&](library::MusicLibrary& storage)
                                        {
                                          id = library::test::addTrackWithUniqueFixtureUri(
                                            storage, {.credits = {{"Alphabet", CreditKind::Performer, "Piano"}}});
                                        }};
    auto& runtime = fixture.runtime();
    auto sections =
      ao::test::requireValue(uimodel::loadTrackCreditsEditorBaseline(runtime.library().snapshot(), std::array{id}));
    auto& scope = fixture.attachTrackDetailScope({.trackIds = {id}, .credits = std::move(sections)});
    // The window outlives the editor and its completion attachments.
    auto windowFixture = GtkWindowFixture{};
    auto editor = TrackCreditsEditor{runtime.async(),
                                     runtime.library(),
                                     runtime.completion(),
                                     runtime.notifications(),
                                     ao::test::englishMessageCatalog(),
                                     &scope,
                                     nullptr};
    windowFixture.mount(editor);
    windowFixture.present();
    auto* edit = findButtonByLabel(editor, "Edit Value");
    REQUIRE(edit != nullptr);
    emitClicked(*edit);
    drainGtkEvents();
    auto* name = findWidgetByClass<Gtk::Entry>(editor, "ao-credit-name");
    auto* role = findWidgetByClass<Gtk::Entry>(editor, "ao-credit-role");
    REQUIRE(name != nullptr);
    REQUIRE(role != nullptr);
    role->set_text("Pi");
    REQUIRE(name->grab_focus());
    name->set_text("Al");
    drainGtkEvents();
    auto* popover = findWidget<Gtk::Popover>(*name);
    REQUIRE(popover != nullptr);
    REQUIRE(popover->get_visible());
    auto* focus = windowFixture.window().get_focus();
    REQUIRE(focus != nullptr);
    REQUIRE((focus == name || focus->is_ancestor(*name)));

    // No main-loop turn separates the snapshot and entry signals.
    scope.setSnapshot(scope.snapshot());
    role->set_text("Pian");
    name->set_text("Alp");
    drainGtkEvents();
    auto* refreshedName = findWidgetByClass<Gtk::Entry>(editor, "ao-credit-name");
    auto* refreshedRole = findWidgetByClass<Gtk::Entry>(editor, "ao-credit-role");
    REQUIRE(refreshedName != nullptr);
    REQUIRE(refreshedRole != nullptr);
    CHECK(refreshedName == name);
    CHECK(refreshedRole == role);
    CHECK(refreshedName->get_text() == "Alp");
    CHECK(refreshedRole->get_text() == "Pian");
    CHECK(windowFixture.window().get_focus() == focus);
    auto* refreshedPopover = findWidget<Gtk::Popover>(*refreshedName);
    REQUIRE(refreshedPopover != nullptr);
    CHECK(refreshedPopover == popover);
    CHECK(refreshedPopover->get_visible());
    auto* save = findWidgetByClass<Gtk::Button>(editor, "ao-credits-commit");
    REQUIRE(save != nullptr);
    REQUIRE(save->get_sensitive());
    bool observedPendingPublication = false;
    bool retainedPendingRows = false;
    bool retainedPendingCompletion = false;
    auto subscription = runtime.library().changes().onChanged(
      [&](rt::LibraryChangeSet const& change)
      {
        if (std::ranges::contains(change.tracksMutated, id))
        {
          scope.setSnapshot(scope.snapshot());
          // Publication cannot wake the submitted command until this returns.
          drainGtkEvents();
          auto* pendingName = findWidgetByClass<Gtk::Entry>(editor, "ao-credit-name");
          retainedPendingRows = pendingName == refreshedName;
          retainedPendingCompletion =
            pendingName != nullptr && findWidget<Gtk::Popover>(*pendingName) == refreshedPopover;
          observedPendingPublication = true;
        }
      });
    emitClicked(*save);
    REQUIRE(tryPumpGtkEventsUntil([&] { return collectAll<Gtk::Entry>(editor).empty(); }));
    subscription.reset();
    CHECK(observedPendingPublication);
    CHECK(retainedPendingRows);
    CHECK(retainedPendingCompletion);
    CHECK(runtime.library().snapshot().trackCredits(id) == std::vector<Credit>{{"Alp", CreditKind::Performer, "Pian"}});
    windowFixture.unmount();
  }

  TEST_CASE("TrackCreditsEditor refresh - committed credits and custom topology preserve a stale live draft",
            "[gtk][integration][track-credits][concurrency]")
  {
    auto id = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.credits_projection_refresh_test",
                                        [&](library::MusicLibrary& storage)
                                        {
                                          id = library::test::addTrackWithUniqueFixtureUri(
                                            storage, {.credits = {{"Alphabet", CreditKind::Performer, "Piano"}}});
                                        }};
    auto& runtime = fixture.runtime();
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    REQUIRE(runtime.views().setSelection(viewId, {id}));
    auto windowFixture = GtkWindowFixture{};
    auto componentPtr = fixture.create(
      uimodel::LayoutNode{.type = "track.detailScope", .children = {uimodel::LayoutNode{.type = "track.fieldGrid"}}});
    REQUIRE(componentPtr != nullptr);
    windowFixture.mount(componentPtr->widget());
    windowFixture.present();
    auto* editor = findWidget<TrackCreditsEditor>(componentPtr->widget());
    REQUIRE(editor != nullptr);
    auto* edit = findButtonByLabel(*editor, "Edit Value");
    REQUIRE(edit != nullptr);
    emitClicked(*edit);
    drainGtkEvents();
    auto* name = findWidgetByClass<Gtk::Entry>(*editor, "ao-credit-name");
    auto* role = findWidgetByClass<Gtk::Entry>(*editor, "ao-credit-role");
    REQUIRE(name != nullptr);
    REQUIRE(role != nullptr);
    role->set_text("Pi");
    REQUIRE(name->grab_focus());
    name->set_text("Al");
    drainGtkEvents();
    auto* popover = findWidget<Gtk::Popover>(*name);
    REQUIRE(popover != nullptr);
    REQUIRE(popover->get_visible());
    auto* focus = windowFixture.window().get_focus();
    REQUIRE(focus != nullptr);
    REQUIRE((focus == name || focus->is_ancestor(*name)));
    bool editedDuringPublication = false;
    bool focusRetainedDuringPublication = false;
    bool completionRetainedDuringPublication = false;
    auto subscription = runtime.library().changes().onChanged(
      [&](rt::LibraryChangeSet const& change)
      {
        if (std::ranges::contains(change.tracksMutated, id))
        {
          focusRetainedDuringPublication = windowFixture.window().get_focus() == focus;
          completionRetainedDuringPublication = popover->get_visible();
          role->set_text("Pian");
          name->set_text("Alp");
          editedDuringPublication = true;
        }
      });
    auto externalSession =
      ao::test::requireValue(uimodel::TrackAuthoringSession::begin(runtime.library(), std::array{id}));
    auto patch = rt::MetadataPatch{};
    patch.optCredits = rt::CreditReplacement{
      .kinds = uimodel::allTrackCreditKinds(), .entries = {{"External", CreditKind::Performer, "Violin"}}};
    patch.customUpdates.emplace("Mood", "Bright");
    auto replyRes = runGtkTask(runtime, externalSession.submitMetadataAsync(std::move(patch)));
    REQUIRE(replyRes);
    REQUIRE(replyRes->status == rt::AuthoringStatus::Applied);
    subscription.reset();
    drainGtkEvents();
    CHECK(editedDuringPublication);
    CHECK(focusRetainedDuringPublication);
    CHECK(completionRetainedDuringPublication);
    auto* refreshedName = findWidgetByClass<Gtk::Entry>(*editor, "ao-credit-name");
    auto* refreshedRole = findWidgetByClass<Gtk::Entry>(*editor, "ao-credit-role");
    REQUIRE(refreshedName != nullptr);
    REQUIRE(refreshedRole != nullptr);
    CHECK(refreshedName == name);
    CHECK(refreshedRole == role);
    CHECK(refreshedName->get_text() == "Alp");
    CHECK(refreshedRole->get_text() == "Pian");
    CHECK(windowFixture.window().get_focus() == focus);
    auto* refreshedPopover = findWidget<Gtk::Popover>(*refreshedName);
    REQUIRE(refreshedPopover != nullptr);
    CHECK(refreshedPopover == popover);
    CHECK(refreshedPopover->get_visible());
    auto* save = findWidgetByClass<Gtk::Button>(*editor, "ao-credits-commit");
    REQUIRE(save != nullptr);
    CHECK_FALSE(save->get_sensitive());
    emitClicked(*save);
    auto const staleText = i18n::requiredText(ao::test::englishMessageCatalog(), i18n::MessageId::TrackEditStale);
    REQUIRE(tryPumpGtkEventsUntil(
      [&]
      {
        return std::ranges::any_of(runtime.notifications().feed().entries,
                                   [&](auto const& notification)
                                   {
                                     auto const* text = std::get_if<std::string>(&notification.message);
                                     return text != nullptr && *text == staleText &&
                                            notification.severity == rt::NotificationSeverity::Error;
                                   });
      }));
    CHECK(runtime.library().snapshot().trackCredits(id) ==
          std::vector<Credit>{{"External", CreditKind::Performer, "Violin"}});
    CHECK(runtime.library().snapshot().trackCustomMetadataValue(id, "Mood") == "Bright");
    windowFixture.unmount();
  }
} // namespace ao::gtk::layout::test
