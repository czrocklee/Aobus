// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/ShellInteractionModel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "tui/Keymap.h"
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace ao::tui::test
{
  TEST_CASE("ShellInteractionModel - input mode and touched state are explicit", "[tui][unit][shell]")
  {
    auto model = ShellInteractionModel{};

    model.beginInput(ShellInputMode::QuickFilter);

    CHECK(model.isInputActive());
    CHECK(model.inputMode() == ShellInputMode::QuickFilter);
    CHECK_FALSE(model.isInputTouched());
    CHECK(model.inputDraft().empty());

    model.insertInputText("detail");

    CHECK(model.isInputTouched());
    CHECK(model.inputDraft() == "detail");

    model.closeInput();

    CHECK_FALSE(model.isInputActive());
    CHECK(model.inputMode() == ShellInputMode::None);
    CHECK_FALSE(model.isInputTouched());
    CHECK(model.inputDraft().empty());
  }

  TEST_CASE("ShellInteractionModel - closing command input clears the draft", "[tui][unit][shell]")
  {
    auto model = ShellInteractionModel{};

    model.beginInput(ShellInputMode::Command, "help");
    model.closeInput();

    CHECK_FALSE(model.isInputActive());
    CHECK(model.inputDraft().empty());
  }

  TEST_CASE("ShellInteractionModel - backspace removes one extended grapheme cluster", "[tui][unit][shell]")
  {
    auto model = ShellInteractionModel{};

    model.beginInput(ShellInputMode::Command);
    model.insertInputText("a翼e\u0301👨‍👩‍👧‍👦");
    model.backspaceInput();

    CHECK(model.inputDraft() == "a翼e\u0301");
    model.backspaceInput();

    CHECK(model.inputDraft() == "a翼");
    model.backspaceInput();

    CHECK(model.inputDraft() == "a");
  }

  TEST_CASE("ShellInteractionModel - overlay state is explicit", "[tui][unit][shell]")
  {
    auto model = ShellInteractionModel{};

    CHECK(model.overlay() == Overlay::None);
    model.openOverlay(Overlay::Help);
    CHECK(model.overlay() == Overlay::Help);
    model.closeOverlay();
    CHECK(model.overlay() == Overlay::None);
  }

  TEST_CASE("ShellInteractionModel - overlay labels are stable", "[tui][unit][shell]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(overlayLabel(textCatalog, Overlay::None) == "Tracks");
    CHECK(overlayLabel(textCatalog, Overlay::QualityPanel) == "Pipeline");
    CHECK(overlayLabel(textCatalog, Overlay::OutputDevices) == "Output");
    CHECK(overlayLabel(textCatalog, Overlay::PresentationPanel) == "Views");
    CHECK(overlayLabel(textCatalog, Overlay::Notifications) == "Notifications");
    CHECK(overlayLabel(textCatalog, Overlay::Help) == "Help");
    CHECK(overlayLabel(textCatalog, Overlay::ListChooser) == "Lists");
    CHECK(overlayLabel(textCatalog, Overlay::GoTo) == "Go to");
  }

  TEST_CASE("ShellInteractionModel - overlay hints are stable", "[tui][unit][shell]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const& keymapPlan = defaultKeymapPlan();
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::None).empty());
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::ListChooser) == "L sidebar  Enter open  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::QualityPanel) == "Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::OutputDevices) == "Enter select  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::PresentationPanel) == "Enter select  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::Notifications) == "x hide compact  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::Help) == "Esc close");
  }

  TEST_CASE("ShellInteractionModel - overlay hints omit access bindings and retain local actions",
            "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({
      {"tui.shell.toggleListChooser", {"Enter", "F2"}},
      {"tui.shell.toggleNotifications", {"X", "F3"}},
    });
    auto const plan = KeymapPlan{model};
    auto const& textCatalog = ao::test::englishMessageCatalog();

    CHECK(plan.shortcutFor(KeyAction::ToggleLists) == "Enter");

    CHECK(plan.shortcutFor(KeyAction::ToggleNotifications) == "x");
    CHECK(overlayHint(textCatalog, plan, Overlay::Notifications) == "x hide compact  Esc close");

    auto outputModel = uimodel::KeymapModel{defaultKeymap()};
    outputModel.applyOverrides({{"tui.shell.toggleOutputDevices", {"Enter"}}});
    auto const outputPlan = KeymapPlan{outputModel};
    CHECK(outputPlan.shortcutFor(KeyAction::ToggleOutputDevices) == "Enter");
    CHECK(overlayHint(textCatalog, outputPlan, Overlay::OutputDevices) == "Enter select  Esc close");
  }
} // namespace ao::tui::test
