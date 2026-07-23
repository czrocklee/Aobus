// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AppDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <glib-object.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("AppDialog - configures a modal window with app header", "[gtk][unit][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto dialog = AppDialog{};

    auto* const titlebar = dialog.get_titlebar();
    REQUIRE(titlebar != nullptr);

    auto* const headerBar = dynamic_cast<Gtk::HeaderBar*>(titlebar);
    REQUIRE(headerBar != nullptr);

    CHECK_FALSE(headerBar->get_show_title_buttons());
    CHECK(dialog.get_modal());
  }

  TEST_CASE("AppDialog - parent and default response use shared dialog behavior", "[gtk][unit][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto parent = Gtk::Window{};
    auto dialog = AppDialog{};

    dialog.configureForParent(parent);
    CHECK(dialog.get_transient_for() == &parent);
    CHECK(dialog.get_destroy_with_parent());
    CHECK(dialog.get_modal());

    dialog.setDefaultResponse(-3);
    auto* const cancelButton = dialog.addCancelAction("Cancel", -6);
    auto* const primaryButton = dialog.addPrimaryAction("Save", -3);
    REQUIRE(cancelButton != nullptr);
    REQUIRE(primaryButton != nullptr);

    CHECK(cancelButton->get_receives_default());
    CHECK(primaryButton->get_receives_default());
    CHECK(dialog.get_default_widget() == primaryButton);
  }

  TEST_CASE("AppDialog - action buttons emit configured response ids", "[gtk][unit][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto dialog = AppDialog{};
    auto responses = std::vector<std::int32_t>{};
    dialog.signal_response().connect([&](std::int32_t const id) { responses.push_back(id); });

    auto* const cancelButton = dialog.addCancelAction("Cancel", -6);
    auto* const primaryButton = dialog.addPrimaryAction("Save", -3);
    REQUIRE(cancelButton != nullptr);
    REQUIRE(primaryButton != nullptr);

    CHECK(primaryButton->has_css_class("suggested-action"));
    CHECK_FALSE(cancelButton->has_css_class("suggested-action"));
    CHECK(findButtonByLabel(dialog.headerBar(), "Cancel") == nullptr);
    CHECK(findButtonByLabel(dialog.headerBar(), "Save") == nullptr);
    CHECK(findButtonByLabel(dialog, "Cancel") == cancelButton);
    CHECK(findButtonByLabel(dialog, "Save") == primaryButton);

    emitClicked(*primaryButton);
    emitClicked(*cancelButton);

    REQUIRE(responses.size() == 2U);
    CHECK(responses[0] == -3);
    CHECK(responses[1] == -6);
  }

  TEST_CASE("AppDialog - response tolerates managed dialog finalization from its handler",
            "[gtk][regression][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto parent = Gtk::Window{};
    auto* const dialog = AppDialog::presentMessage(
      parent,
      "Finished",
      "This dialog closes from its response handler.",
      {AppDialogAction{.label = "OK", .responseId = -5, .role = AppDialogActionRole::Primary}},
      -5);
    bool finalized = false;
    auto const markFinalized = +[](void* const data, GObject*) { *static_cast<bool*>(data) = true; };
    ::g_object_weak_ref(G_OBJECT(dialog->gobj()), markFinalized, &finalized);

    dialog->response(-5);
    drainGtkEvents();

    CHECK(finalized);

    if (!finalized)
    {
      ::g_object_weak_unref(G_OBJECT(dialog->gobj()), markFinalized, &finalized);
    }
  }

  TEST_CASE("AppDialog - content replacement detaches previous widget", "[gtk][unit][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto dialog = AppDialog{};
    auto first = Gtk::Label{"First content"};
    auto second = Gtk::Label{"Second content"};

    dialog.setContentWidget(first);
    REQUIRE(first.get_parent() != nullptr);
    CHECK(first.get_hexpand());
    CHECK(first.get_vexpand());
    CHECK(findLabelByText(dialog, "First content") == &first);

    dialog.setContentWidget(second);

    CHECK(first.get_parent() == nullptr);
    REQUIRE(second.get_parent() != nullptr);
    CHECK(second.get_hexpand());
    CHECK(second.get_vexpand());
    CHECK(findLabelByText(dialog, "First content") == nullptr);
    CHECK(findLabelByText(dialog, "Second content") == &second);
  }

  TEST_CASE("AppDialog - message factory builds standard modal response dialogs", "[gtk][unit][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto parent = Gtk::Window{};
    auto dialogPtr = AppDialog::createMessage(
      parent,
      "Confirm Change",
      "This action updates the saved layout.",
      {AppDialogAction{.label = "No", .responseId = -9, .role = AppDialogActionRole::Cancel},
       AppDialogAction{.label = "Yes", .responseId = -8, .role = AppDialogActionRole::Primary}},
      -9);

    REQUIRE(dialogPtr != nullptr);
    CHECK(dialogPtr->get_title() == "Confirm Change");
    CHECK(dialogPtr->get_transient_for() == &parent);
    CHECK(dialogPtr->get_modal());
    CHECK(findLabelByText(*dialogPtr, "This action updates the saved layout.") != nullptr);

    auto* const noButton = findButtonByLabel(*dialogPtr, "No");
    auto* const yesButton = findButtonByLabel(*dialogPtr, "Yes");
    REQUIRE(noButton != nullptr);
    REQUIRE(yesButton != nullptr);
    CHECK(dialogPtr->get_default_widget() == noButton);
    CHECK(yesButton->has_css_class("suggested-action"));

    auto responses = std::vector<std::int32_t>{};
    dialogPtr->signal_response().connect([&](std::int32_t const id) { responses.push_back(id); });

    emitClicked(*yesButton);

    REQUIRE(responses.size() == 1U);
    CHECK(responses.front() == -8);
  }
} // namespace ao::gtk::test
