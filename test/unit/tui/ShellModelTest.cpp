// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/ShellModel.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::tui::test
{
  TEST_CASE("ShellModel - command parser recognizes terminal app commands", "[tui][unit][shell]")
  {
    CHECK(parseCommand("/lists").action == CommandAction::OpenLists);
    CHECK(parseCommand(":detail").action == CommandAction::OpenDetail);
    CHECK(parseCommand("/quality").action == CommandAction::OpenQuality);
    CHECK(parseCommand("/pipeline").action == CommandAction::OpenQuality);
    CHECK(parseCommand("/output").action == CommandAction::OpenOutputDevices);
    CHECK(parseCommand("/devices").action == CommandAction::OpenOutputDevices);
    CHECK(parseCommand("/views").action == CommandAction::OpenPresentationPanel);
    CHECK(parseCommand("/v").action == CommandAction::OpenPresentationPanel);
    CHECK(parseCommand("help").action == CommandAction::ShowHelp);
    CHECK(parseCommand("/current").action == CommandAction::RevealCurrentTrack);
    auto presentationCommand = parseCommand("/view albums");
    CHECK(presentationCommand.action == CommandAction::SetPresentation);
    CHECK(presentationCommand.argument == "albums");
    presentationCommand = parseCommand("/presentation tagging");
    CHECK(presentationCommand.action == CommandAction::SetPresentation);
    CHECK(presentationCommand.argument == "tagging");
    presentationCommand = parseCommand("/preset Albums");
    CHECK(presentationCommand.action == CommandAction::SetPresentation);
    CHECK(presentationCommand.argument == "Albums");
    CHECK(parseCommand("now").action == CommandAction::RevealCurrentTrack);
    CHECK(parseCommand("reveal").action == CommandAction::RevealCurrentTrack);
    CHECK(parseCommand("clear").action == CommandAction::ClearFilter);
    CHECK(parseCommand("reload").action == CommandAction::Reload);
    CHECK(parseCommand("play").action == CommandAction::Play);
    CHECK(parseCommand("pause").action == CommandAction::TogglePlayback);
    CHECK(parseCommand("stop").action == CommandAction::Stop);
    CHECK(parseCommand("quit").action == CommandAction::Quit);
    CHECK(parseCommand("close").action == CommandAction::CloseOverlay);
  }

  TEST_CASE("ShellModel - unknown commands become quick filters", "[tui][unit][shell]")
  {
    auto command = parseCommand("/aimer midnight");

    CHECK(command.action == CommandAction::QuickFilter);
    CHECK(command.argument == "aimer midnight");

    command = parseCommand("/filter live acoustic");

    CHECK(command.action == CommandAction::QuickFilter);
    CHECK(command.argument == "live acoustic");

    command = parseCommand("  :filter   spaced query   ");

    CHECK(command.action == CommandAction::QuickFilter);
    CHECK(command.argument == "spaced query");
  }

  TEST_CASE("ShellModel - command draft lifecycle submits and clears", "[tui][unit][shell]")
  {
    auto model = ShellModel{};

    model.beginCommand();
    model.appendCommandText("detail");

    CHECK(model.commandActive());
    CHECK(model.commandDraft() == "detail");

    auto command = model.submitCommand();

    CHECK(command.action == CommandAction::OpenDetail);
    CHECK_FALSE(model.commandActive());
    CHECK(model.commandDraft().empty());
  }

  TEST_CASE("ShellModel - cancelling command input clears the draft", "[tui][unit][shell]")
  {
    auto model = ShellModel{};

    model.beginCommand("help");
    model.cancelCommand();

    CHECK_FALSE(model.commandActive());
    CHECK(model.commandDraft().empty());
  }

  TEST_CASE("ShellModel - backspace removes one UTF-8 codepoint", "[tui][unit][shell]")
  {
    auto model = ShellModel{};

    model.beginCommand();
    model.appendCommandText("a翼");
    model.backspaceCommand();

    CHECK(model.commandDraft() == "a");
  }

  TEST_CASE("ShellModel - overlay state is explicit", "[tui][unit][shell]")
  {
    auto model = ShellModel{};

    CHECK(model.overlay() == Overlay::None);
    model.openOverlay(Overlay::ListChooser);
    CHECK(model.overlay() == Overlay::ListChooser);
    model.closeOverlay();
    CHECK(model.overlay() == Overlay::None);
  }

  TEST_CASE("ShellModel - overlay labels are stable", "[tui][unit][shell]")
  {
    CHECK(overlayLabel(Overlay::None) == "Tracks");
    CHECK(overlayLabel(Overlay::ListChooser) == "Lists");
    CHECK(overlayLabel(Overlay::DetailPanel) == "Detail");
    CHECK(overlayLabel(Overlay::QualityPanel) == "Quality");
    CHECK(overlayLabel(Overlay::OutputDevices) == "Output");
    CHECK(overlayLabel(Overlay::PresentationPanel) == "Views");
    CHECK(overlayLabel(Overlay::Help) == "Help");
  }

  TEST_CASE("ShellModel - overlay hints are stable", "[tui][unit][shell]")
  {
    CHECK(overlayHint(Overlay::None) ==
          "/ command  l lists  v view  d detail  a quality  o output  Ctrl-L current  q quit");
    CHECK(overlayHint(Overlay::ListChooser) == "l toggle  Enter open  Esc close");
    CHECK(overlayHint(Overlay::DetailPanel) == "d toggle  Esc close");
    CHECK(overlayHint(Overlay::QualityPanel) == "a toggle  Esc close");
    CHECK(overlayHint(Overlay::OutputDevices) == "o toggle  Enter select  Esc close");
    CHECK(overlayHint(Overlay::PresentationPanel) == "v toggle  Enter select  Esc close");
    CHECK(overlayHint(Overlay::Help) == "Esc close");
  }
} // namespace ao::tui::test
