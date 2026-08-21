// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/ShellInteractionModel.h"

#include "test/unit/tui/TuiTextCatalogTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::tui::test
{
  TEST_CASE("ShellInteractionModel - command parser recognizes terminal app commands", "[tui][unit][shell]")
  {
    CHECK(parseCommand("/lists").action == CommandAction::OpenLists);
    CHECK(parseCommand(":detail").action == CommandAction::OpenDetail);
    CHECK(parseCommand("/quality").action == CommandAction::OpenQuality);
    CHECK(parseCommand("/pipeline").action == CommandAction::OpenQuality);
    CHECK(parseCommand("/output").action == CommandAction::OpenOutputDevices);
    CHECK(parseCommand("/devices").action == CommandAction::OpenOutputDevices);
    CHECK(parseCommand("/views").action == CommandAction::OpenPresentationPanel);
    CHECK(parseCommand("/v").action == CommandAction::OpenPresentationPanel);
    CHECK(parseCommand("/notifications").action == CommandAction::OpenNotifications);
    CHECK(parseCommand("/n").action == CommandAction::OpenNotifications);
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

  TEST_CASE("ShellInteractionModel - unknown commands become quick filters", "[tui][unit][shell]")
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

  TEST_CASE("ShellInteractionModel - command draft lifecycle submits and clears", "[tui][unit][shell]")
  {
    auto model = ShellInteractionModel{};

    model.beginCommand();
    model.appendCommandText("detail");

    CHECK(model.isCommandActive());
    CHECK(model.commandDraft() == "detail");

    auto command = model.submitCommand();

    CHECK(command.action == CommandAction::OpenDetail);
    CHECK_FALSE(model.isCommandActive());
    CHECK(model.commandDraft().empty());
  }

  TEST_CASE("ShellInteractionModel - cancelling command input clears the draft", "[tui][unit][shell]")
  {
    auto model = ShellInteractionModel{};

    model.beginCommand("help");
    model.cancelCommand();

    CHECK_FALSE(model.isCommandActive());
    CHECK(model.commandDraft().empty());
  }

  TEST_CASE("ShellInteractionModel - backspace removes one extended grapheme cluster", "[tui][unit][shell]")
  {
    auto model = ShellInteractionModel{};

    model.beginCommand();
    model.appendCommandText("a翼e\u0301👨‍👩‍👧‍👦");
    model.backspaceCommand();

    CHECK(model.commandDraft() == "a翼e\u0301");
    model.backspaceCommand();

    CHECK(model.commandDraft() == "a翼");
    model.backspaceCommand();

    CHECK(model.commandDraft() == "a");
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
    auto const& textCatalog = englishTuiTextCatalog();
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
    auto const& textCatalog = englishTuiTextCatalog();
    CHECK(overlayHint(textCatalog, Overlay::None) ==
          "/ command  l lists  v view  n notif  d detail  a pipeline  o output  { } groups  Ctrl-L current  q quit");
    CHECK(overlayHint(textCatalog, Overlay::ListChooser) == "l toggle  Enter open  Esc close");
    CHECK(overlayHint(textCatalog, Overlay::DetailPanel) == "d toggle  Esc close");
    CHECK(overlayHint(textCatalog, Overlay::QualityPanel) == "a toggle  Esc close");
    CHECK(overlayHint(textCatalog, Overlay::OutputDevices) == "o toggle  Enter select  Esc close");
    CHECK(overlayHint(textCatalog, Overlay::PresentationPanel) == "v toggle  Enter select  Esc close");
    CHECK(overlayHint(textCatalog, Overlay::Notifications) == "n toggle  x hide compact  Esc close");
    CHECK(overlayHint(textCatalog, Overlay::Help) == "Esc close");
  }
} // namespace ao::tui::test
