// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/ShellInteractionModel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "tui/Keymap.h"
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

#include <cstddef>
#include <string>
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
    CHECK_FALSE(isModalOverlay(Overlay::None));

    for (auto const overlay : {Overlay::QualityPanel,
                               Overlay::OutputDevices,
                               Overlay::PresentationPanel,
                               Overlay::Notifications,
                               Overlay::Help,
                               Overlay::GoTo,
                               Overlay::ListChooser})
    {
      CHECK(isModalOverlay(overlay));
    }

    model.openOverlay(Overlay::Help);
    CHECK(model.overlay() == Overlay::Help);
    model.closeOverlay();
    CHECK(model.overlay() == Overlay::None);
  }

  TEST_CASE("ShellInteractionModel - replacing an overlay resets its scroll position", "[tui][unit][shell]")
  {
    auto model = ShellInteractionModel{};
    model.openOverlay(Overlay::Help);
    model.scrollOverlay(4, 10);
    CHECK(model.overlayScroll() == 4);
    model.openOverlay(Overlay::OutputDevices);
    CHECK(model.overlay() == Overlay::OutputDevices);
    CHECK(model.overlayScroll() == 0);
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

  TEST_CASE("ShellInteractionModel - panel-owned overlay hints are stable", "[tui][unit][shell]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const& keymapPlan = defaultKeymapPlan();
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::None).empty());
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::Notifications).empty());
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::Help).empty());
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::ListChooser) == "L sidebar  Enter open  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::QualityPanel) == "Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::OutputDevices) == "Enter select  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::PresentationPanel) == "Enter select  Esc close");
  }

  TEST_CASE("ShellInteractionModel - overlay hints omit access bindings and retain local actions",
            "[tui][unit][shell][keymap]")
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

    auto outputModel = uimodel::KeymapModel{defaultKeymap()};
    outputModel.applyOverrides({{"tui.shell.toggleOutputDevices", {"Enter"}}});
    auto const outputPlan = KeymapPlan{outputModel};
    CHECK(outputPlan.shortcutFor(KeyAction::ToggleOutputDevices) == "Enter");
    CHECK(overlayHint(textCatalog, outputPlan, Overlay::OutputDevices) == "Enter select  Esc close");

    auto distinctModel = uimodel::KeymapModel{defaultKeymap()};
    distinctModel.applyOverrides({{"tui.shell.toggleListChooser", {"F2"}}, {"tui.shell.toggleOutputDevices", {"F3"}}});
    auto const distinctPlan = KeymapPlan{distinctModel};
    CHECK(distinctPlan.shortcutFor(KeyAction::ToggleLists) == "F2");
    CHECK(distinctPlan.shortcutFor(KeyAction::ToggleOutputDevices) == "F3");
    CHECK_FALSE(overlayHint(textCatalog, distinctPlan, Overlay::ListChooser).contains("F2"));
    CHECK(overlayHint(textCatalog, distinctPlan, Overlay::ListChooser).contains("Enter open"));
    CHECK(overlayHint(textCatalog, distinctPlan, Overlay::OutputDevices) == "Enter select  Esc close");
  }

  TEST_CASE("ShellInteractionModel - deleting an empty line does not mark an untouched filter as edited",
            "[tui][unit][shell][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::QuickFilter);
    CHECK_FALSE(shell.tryEditInput(ftxui::Event::CtrlU));
    CHECK_FALSE(shell.tryEditInput(ftxui::Event::CtrlK));
    CHECK_FALSE(shell.isInputTouched());
    REQUIRE(shell.tryEditInput(ftxui::Event::Character("one two")));
    CHECK_FALSE(shell.tryEditInput(ftxui::Event::ArrowLeft));
    CHECK(shell.inputDraft() == "one two");
    CHECK(shell.inputField().cursor() == 6);
    REQUIRE(shell.tryEditInput(ftxui::Event::CtrlU));
    CHECK(shell.inputDraft() == "o");
    REQUIRE(shell.tryEditInput(ftxui::Event::CtrlK));
    CHECK(shell.inputDraft().empty());
  }

  TEST_CASE("ShellInteractionModel - history is mode-local and restores the unfinished draft",
            "[tui][unit][shell][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "detail");
    shell.rememberInput();
    shell.beginInput(ShellInputMode::Command, "settings");
    shell.rememberInput();
    shell.beginInput(ShellInputMode::QuickFilter, "artist");
    shell.rememberInput();
    shell.beginInput(ShellInputMode::Command, "unsent");
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "settings");
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "detail");
    CHECK_FALSE(shell.tryMoveInputHistory(-1));
    REQUIRE(shell.tryMoveInputHistory(1));
    REQUIRE(shell.tryMoveInputHistory(1));
    CHECK(shell.inputDraft() == "unsent");
    shell.beginInput(ShellInputMode::QuickFilter);
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "artist");
  }

  TEST_CASE("ShellInteractionModel - history discards empty drafts, deduplicates and retains newest fifty",
            "[tui][unit][shell][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command);
    shell.rememberInput();
    CHECK_FALSE(shell.tryMoveInputHistory(-1));

    for (std::size_t index = 0; index < 51; ++index)
    {
      shell.beginInput(ShellInputMode::Command, "entry " + std::to_string(index));
      shell.rememberInput();
    }

    shell.beginInput(ShellInputMode::Command, "entry 12");
    shell.rememberInput();
    shell.beginInput(ShellInputMode::Command, "draft");
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "entry 12");
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "entry 50");

    for (std::size_t index = 0; index < 48; ++index)
    {
      REQUIRE(shell.tryMoveInputHistory(-1));
    }

    CHECK(shell.inputDraft() == "entry 1");
    CHECK_FALSE(shell.tryMoveInputHistory(-1));
    REQUIRE(shell.tryMoveInputHistory(50));
    CHECK(shell.inputDraft() == "draft");
  }

  TEST_CASE("ShellInteractionModel - editing a history entry creates a new scratch draft", "[tui][unit][shell][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "filter A");
    shell.rememberInput();
    shell.beginInput(ShellInputMode::Command);
    REQUIRE(shell.tryMoveInputHistory(-1));
    REQUIRE(shell.tryEditInput(ftxui::Event::Character("B")));
    CHECK_FALSE(shell.tryMoveInputHistory(1));
    REQUIRE(shell.tryMoveInputHistory(-1));
    CHECK(shell.inputDraft() == "filter A");
    REQUIRE(shell.tryMoveInputHistory(1));
    CHECK(shell.inputDraft() == "filter AB");
  }

  TEST_CASE("ShellInteractionModel - completion applies only its interior range and retires the result",
            "[tui][unit][shell][input]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "filter $ar = Aimer");
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 7,
      .replaceEnd = 10,
      .items = {rt::CompletionItem{.displayText = "$artist", .insertText = "$artist"}},
    });
    REQUIRE(shell.tryApplyCommandCompletion());
    CHECK(shell.inputDraft() == "filter $artist = Aimer");
    CHECK(shell.inputField().cursor() == 14);
    CHECK_FALSE(shell.commandCompletion());
  }
} // namespace ao::tui::test
