// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/ShellInteractionModel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/TuiKeymapTestSupport.h"
#include "tui/TuiKeymap.h"
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <utility>

namespace ao::tui::test
{
  namespace
  {
    Command requiredCommand(std::string_view const input)
    {
      auto const optCommand = parseCommand(input);
      REQUIRE(optCommand);
      return *optCommand;
    }
  } // namespace

  TEST_CASE("ShellInteractionModel - command parser recognizes terminal app commands", "[tui][unit][shell]")
  {
    CHECK(requiredCommand(":lists").action == CommandAction::OpenLists);
    CHECK(requiredCommand(":detail").action == CommandAction::OpenDetail);
    CHECK(requiredCommand(":quality").action == CommandAction::OpenQuality);
    CHECK(requiredCommand(":pipeline").action == CommandAction::OpenQuality);
    CHECK(requiredCommand(":output").action == CommandAction::OpenOutputDevices);
    CHECK(requiredCommand(":devices").action == CommandAction::OpenOutputDevices);
    CHECK(requiredCommand(":views").action == CommandAction::OpenPresentationPanel);
    CHECK(requiredCommand(":v").action == CommandAction::OpenPresentationPanel);
    CHECK(requiredCommand(":notifications").action == CommandAction::OpenNotifications);
    CHECK(requiredCommand(":n").action == CommandAction::OpenNotifications);
    CHECK(requiredCommand("help").action == CommandAction::ShowHelp);
    CHECK(requiredCommand(":current").action == CommandAction::RevealCurrentTrack);
    auto presentationCommand = requiredCommand(":view albums");
    CHECK(presentationCommand.action == CommandAction::SetPresentation);
    CHECK(presentationCommand.argument == "albums");
    presentationCommand = requiredCommand(":presentation tagging");
    CHECK(presentationCommand.action == CommandAction::SetPresentation);
    CHECK(presentationCommand.argument == "tagging");
    presentationCommand = requiredCommand(":preset Albums");
    CHECK(presentationCommand.action == CommandAction::SetPresentation);
    CHECK(presentationCommand.argument == "Albums");
    CHECK(requiredCommand("now").action == CommandAction::RevealCurrentTrack);
    CHECK(requiredCommand("reveal").action == CommandAction::RevealCurrentTrack);
    CHECK(requiredCommand("clear").action == CommandAction::ClearFilter);
    CHECK(requiredCommand("reload").action == CommandAction::Reload);
    CHECK(requiredCommand("scan").action == CommandAction::Scan);
    CHECK(requiredCommand("rescan").action == CommandAction::Scan);
    CHECK(requiredCommand("scan cancel").action == CommandAction::ScanCancel);
    CHECK(requiredCommand(":scan cancel").action == CommandAction::ScanCancel);
    CHECK(requiredCommand("select toggle").action == CommandAction::SelectToggle);
    CHECK(requiredCommand("select range").action == CommandAction::SelectRange);
    CHECK(requiredCommand("select all").action == CommandAction::SelectAll);
    CHECK(requiredCommand("select clear").action == CommandAction::SelectClear);
    CHECK(requiredCommand("play").action == CommandAction::Play);
    CHECK(requiredCommand("pause").action == CommandAction::TogglePlayback);
    CHECK(requiredCommand("stop").action == CommandAction::Stop);
    CHECK(requiredCommand("quit").action == CommandAction::Quit);
    CHECK(requiredCommand("close").action == CommandAction::CloseOverlay);
  }

  TEST_CASE("ShellInteractionModel - command and root key actions share one relation", "[tui][unit][keymap]")
  {
    constexpr auto kRelations = std::to_array<std::pair<CommandAction, TuiKeyAction>>({
      {CommandAction::OpenLists, TuiKeyAction::ToggleListChooser},
      {CommandAction::OpenDetail, TuiKeyAction::ToggleDetails},
      {CommandAction::OpenQuality, TuiKeyAction::ToggleAudioPipeline},
      {CommandAction::OpenOutputDevices, TuiKeyAction::ToggleOutputDevices},
      {CommandAction::OpenPresentationPanel, TuiKeyAction::TogglePresentations},
      {CommandAction::OpenNotifications, TuiKeyAction::ToggleNotifications},
      {CommandAction::ShowHelp, TuiKeyAction::ShowHelp},
      {CommandAction::RevealCurrentTrack, TuiKeyAction::RevealCurrentTrack},
      {CommandAction::ClearFilter, TuiKeyAction::ClearFilter},
      {CommandAction::Reload, TuiKeyAction::Reload},
      {CommandAction::Scan, TuiKeyAction::Scan},
      {CommandAction::ScanCancel, TuiKeyAction::ScanCancel},
      {CommandAction::SelectToggle, TuiKeyAction::SelectToggle},
      {CommandAction::SelectRange, TuiKeyAction::SelectRange},
      {CommandAction::SelectAll, TuiKeyAction::SelectAll},
      {CommandAction::SelectClear, TuiKeyAction::SelectClear},
      {CommandAction::Play, TuiKeyAction::PlaySelection},
      {CommandAction::TogglePlayback, TuiKeyAction::PlaybackPlayPause},
      {CommandAction::Stop, TuiKeyAction::PlaybackStop},
      {CommandAction::Quit, TuiKeyAction::Quit},
    });

    for (auto const& [command, key] : kRelations)
    {
      CHECK(shortcutActionForCommand(command) == key);
      CHECK(commandActionForKeyAction(key) == command);
    }

    CHECK_FALSE(shortcutActionForCommand(CommandAction::QuickFilter));
    CHECK_FALSE(shortcutActionForCommand(CommandAction::CloseOverlay));
    CHECK_FALSE(shortcutActionForCommand(CommandAction::SetPresentation));
    CHECK_FALSE(commandActionForKeyAction(TuiKeyAction::OpenCommandPalette));
    CHECK_FALSE(commandActionForKeyAction(TuiKeyAction::OpenQuickFilter));
    CHECK_FALSE(commandActionForKeyAction(TuiKeyAction::PreviousSection));
  }

  TEST_CASE("ShellInteractionModel - only explicit filter commands become quick filters", "[tui][unit][shell]")
  {
    CHECK_FALSE(parseCommand("/aimer midnight"));
    CHECK_FALSE(parseCommand("/lists"));
    CHECK_FALSE(parseCommand("/filter live acoustic"));
    CHECK_FALSE(parseCommand("unknown"));
    CHECK_FALSE(parseCommand("scan foo"));
    CHECK_FALSE(parseCommand("scan  cancel"));
    CHECK_FALSE(parseCommand("select"));
    CHECK_FALSE(parseCommand("  "));

    auto command = requiredCommand(":filter live acoustic");
    CHECK(command.action == CommandAction::QuickFilter);
    CHECK(command.argument == "live acoustic");

    command = requiredCommand("  :filter   spaced query   ");

    CHECK(command.action == CommandAction::QuickFilter);
    CHECK(command.argument == "spaced query");
  }

  TEST_CASE("ShellInteractionModel - input mode and touched state are explicit", "[tui][unit][shell]")
  {
    auto model = ShellInteractionModel{};

    model.beginInput(ShellInputMode::QuickFilter);

    CHECK(model.isInputActive());
    CHECK(model.inputMode() == ShellInputMode::QuickFilter);
    CHECK_FALSE(model.isInputTouched());
    CHECK(model.inputDraft().empty());

    model.appendInputText("detail");

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
    model.appendInputText("a翼e\u0301👨‍👩‍👧‍👦");
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
    model.openOverlay(Overlay::ListChooser);
    CHECK(model.overlay() == Overlay::ListChooser);
    model.closeOverlay();
    CHECK(model.overlay() == Overlay::None);
  }

  TEST_CASE("ShellInteractionModel - overlay labels are stable", "[tui][unit][shell]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(overlayLabel(textCatalog, Overlay::None) == "Tracks");
    CHECK(overlayLabel(textCatalog, Overlay::ListChooser) == "Lists");
    CHECK(overlayLabel(textCatalog, Overlay::DetailPanel) == "Detail");
    CHECK(overlayLabel(textCatalog, Overlay::QualityPanel) == "Pipeline");
    CHECK(overlayLabel(textCatalog, Overlay::OutputDevices) == "Output");
    CHECK(overlayLabel(textCatalog, Overlay::PresentationPanel) == "Views");
    CHECK(overlayLabel(textCatalog, Overlay::Notifications) == "Notifications");
    CHECK(overlayLabel(textCatalog, Overlay::Help) == "Help");
  }

  TEST_CASE("ShellInteractionModel - overlay hints are stable", "[tui][unit][shell]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const& keymapPlan = defaultTuiKeymapPlan();
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::None).empty());
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::ListChooser) == "l toggle  Enter open  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::DetailPanel) == "d toggle  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::QualityPanel) == "a toggle  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::OutputDevices) == "o toggle  Enter select  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::PresentationPanel) == "v toggle  Enter select  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::Notifications) == "n toggle  x hide compact  Esc close");
    CHECK(overlayHint(textCatalog, keymapPlan, Overlay::Help) == "Esc close");
  }

  TEST_CASE("ShellInteractionModel - overlay hints choose a binding that survives local protocol",
            "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{tuiDefaultKeymap()};
    model.applyOverrides({
      {"tui.shell.toggleListChooser", {"Enter", "F2"}},
      {"tui.shell.toggleNotifications", {"X", "F3"}},
    });
    auto const plan = TuiKeymapPlan{model};
    auto const& textCatalog = ao::test::englishMessageCatalog();

    CHECK(plan.shortcutFor(TuiKeyAction::ToggleListChooser) == "Enter");
    CHECK(overlayToggleShortcut(plan, Overlay::ListChooser) == "F2");
    CHECK(overlayHint(textCatalog, plan, Overlay::ListChooser) == "F2 toggle  Enter open  Esc close");

    CHECK(plan.shortcutFor(TuiKeyAction::ToggleNotifications) == "x");
    CHECK(overlayToggleShortcut(plan, Overlay::Notifications) == "F3");
    CHECK(overlayHint(textCatalog, plan, Overlay::Notifications) == "F3 toggle  x hide compact  Esc close");

    auto outputModel = uimodel::KeymapModel{tuiDefaultKeymap()};
    outputModel.applyOverrides({{"tui.shell.toggleOutputDevices", {"Enter"}}});
    auto const outputPlan = TuiKeymapPlan{outputModel};
    CHECK(outputPlan.shortcutFor(TuiKeyAction::ToggleOutputDevices) == "Enter");
    CHECK(overlayToggleShortcut(outputPlan, Overlay::OutputDevices).empty());
    CHECK(overlayHint(textCatalog, outputPlan, Overlay::OutputDevices) == "Enter select  Esc close");
  }
} // namespace ao::tui::test
