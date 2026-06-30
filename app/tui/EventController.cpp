// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EventController.h"

#include "LibraryController.h"
#include "PlaybackActions.h"
#include "ShellModel.h"

#include <ftxui/component/event.hpp>

#include <chrono>
#include <cstdint>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kPageSelectionDelta = 10;
    constexpr std::int32_t kBoundarySelectionDelta = 1'000'000;
    constexpr auto kKeyboardSeekDelta = std::chrono::seconds{5};
    constexpr float kKeyboardVolumeDelta = 0.05F;
  } // namespace

  EventController::EventController(ftxui::ScreenInteractive& screen,
                                   ShellModel& shell,
                                   LibraryController& library,
                                   rt::PlaybackService& playback)
    : _screen{screen}, _shell{shell}, _library{library}, _playback{playback}
  {
  }

  void EventController::openSelectedList()
  {
    auto const result = _library.openSelectedList();

    if (result.opened)
    {
      _shell.closeOverlay();
    }

    _statusMessage = result.status;
  }

  void EventController::reloadActiveList()
  {
    _statusMessage = _library.reloadActiveList();
  }

  void EventController::applyFilter()
  {
    _statusMessage = _library.applyFilter();
  }

  void EventController::toggleDetailPanel()
  {
    if (_shell.overlay() == Overlay::DetailPanel)
    {
      _shell.closeOverlay();
      _statusMessage = "Detail closed";
      return;
    }

    _shell.openOverlay(Overlay::DetailPanel);
    _statusMessage = "Detail panel";
  }

  void EventController::toggleQualityPanel()
  {
    if (_shell.overlay() == Overlay::QualityPanel)
    {
      _shell.closeOverlay();
      _statusMessage = "Quality closed";
      return;
    }

    _shell.openOverlay(Overlay::QualityPanel);
    _statusMessage = "Audio quality";
  }

  void EventController::runCommand(Command const& command)
  {
    switch (command.action)
    {
      case CommandAction::QuickFilter:
        _library.setFilterDraft(command.argument);
        applyFilter();
        break;
      case CommandAction::OpenLists:
        _shell.openOverlay(Overlay::ListChooser);
        _statusMessage = "Lists";
        break;
      case CommandAction::OpenDetail: toggleDetailPanel(); break;
      case CommandAction::OpenQuality: toggleQualityPanel(); break;
      case CommandAction::CloseOverlay:
        _shell.closeOverlay();
        _statusMessage = "Overlay closed";
        break;
      case CommandAction::ShowHelp:
        _shell.openOverlay(Overlay::Help);
        _statusMessage = "Help";
        break;
      case CommandAction::ClearFilter:
        _library.clearFilterDraft();
        applyFilter();
        break;
      case CommandAction::Reload: reloadActiveList(); break;
      case CommandAction::Play:
        _statusMessage = playSelected(_playback, _library.tracks(), _library.selectedTrack(), _library.currentListId())
                           ? "Playback requested"
                           : "Playback did not start. Check output device, file path, and logs.";
        break;
      case CommandAction::TogglePlayback:
        _statusMessage =
          togglePlayback(_playback, _library.tracks(), _library.selectedTrack(), _library.currentListId())
            ? "Playback toggled"
            : "Playback did not start. Check output device, file path, and logs.";
        break;
      case CommandAction::Stop:
        _playback.stop();
        _statusMessage = "Stopped";
        break;
      case CommandAction::Quit:
        _playback.stop();
        _screen.ExitLoopClosure()();
        break;
    }
  }

  // TUI key dispatch is intentionally flat so shortcuts stay visible in one place.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  bool EventController::handleEvent(ftxui::Event const& event)
  {
    if (_shell.commandActive())
    {
      if (event == ftxui::Event::Escape)
      {
        _shell.cancelCommand();
        _statusMessage = "Command cancelled";
        return true;
      }

      if (event == ftxui::Event::Return)
      {
        runCommand(_shell.submitCommand());
        return true;
      }

      if (event == ftxui::Event::Backspace)
      {
        _shell.backspaceCommand();
        return true;
      }

      if (event.is_character())
      {
        _shell.appendCommandText(event.character());
        return true;
      }

      return true;
    }

    if (event == ftxui::Event::Escape)
    {
      _shell.closeOverlay();
      _statusMessage = "Overlay closed";
      return true;
    }

    if (event == ftxui::Event::Character("q") || event == ftxui::Event::CtrlC)
    {
      _playback.stop();
      _screen.ExitLoopClosure()();
      return true;
    }

    if (event == ftxui::Event::ArrowUp)
    {
      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, -1);
      return true;
    }

    if (event == ftxui::Event::ArrowDown)
    {
      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, 1);
      return true;
    }

    if (event == ftxui::Event::PageUp)
    {
      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, -kPageSelectionDelta);
      return true;
    }

    if (event == ftxui::Event::PageDown)
    {
      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, kPageSelectionDelta);
      return true;
    }

    if (event == ftxui::Event::Home)
    {
      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, -kBoundarySelectionDelta);
      return true;
    }

    if (event == ftxui::Event::End)
    {
      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, kBoundarySelectionDelta);
      return true;
    }

    if (event == ftxui::Event::Character("/") || event == ftxui::Event::Character(":"))
    {
      _shell.beginCommand();
      _statusMessage = "Command input";
      return true;
    }

    if (event == ftxui::Event::Character("l"))
    {
      _shell.openOverlay(Overlay::ListChooser);
      _statusMessage = "Lists";
      return true;
    }

    if (event == ftxui::Event::Character("d"))
    {
      toggleDetailPanel();
      return true;
    }

    if (event == ftxui::Event::Character("a"))
    {
      toggleQualityPanel();
      return true;
    }

    if (event == ftxui::Event::Character("?"))
    {
      _shell.openOverlay(Overlay::Help);
      _statusMessage = "Help";
      return true;
    }

    if (event == ftxui::Event::Character("c"))
    {
      _library.clearFilterDraft();
      applyFilter();
      return true;
    }

    if (event == ftxui::Event::Character("r"))
    {
      reloadActiveList();
      return true;
    }

    if (event == ftxui::Event::Return)
    {
      if (_shell.overlay() == Overlay::ListChooser)
      {
        openSelectedList();
        return true;
      }

      _statusMessage = playSelected(_playback, _library.tracks(), _library.selectedTrack(), _library.currentListId())
                         ? "Playback requested"
                         : "Playback did not start. Check output device, file path, and logs.";
      return true;
    }

    if (event == ftxui::Event::Character("p"))
    {
      _statusMessage = playSelected(_playback, _library.tracks(), _library.selectedTrack(), _library.currentListId())
                         ? "Playback requested"
                         : "Playback did not start. Check output device, file path, and logs.";
      return true;
    }

    if (event == ftxui::Event::Character(" "))
    {
      _statusMessage = togglePlayback(_playback, _library.tracks(), _library.selectedTrack(), _library.currentListId())
                         ? "Playback toggled"
                         : "Playback did not start. Check output device, file path, and logs.";
      return true;
    }

    if (event == ftxui::Event::Character("s"))
    {
      _playback.stop();
      _statusMessage = "Stopped";
      return true;
    }

    if (event == ftxui::Event::Character("["))
    {
      seekBy(_playback, -kKeyboardSeekDelta);
      return true;
    }

    if (event == ftxui::Event::Character("]"))
    {
      seekBy(_playback, kKeyboardSeekDelta);
      return true;
    }

    if (event == ftxui::Event::Character("-"))
    {
      changeVolume(_playback, -kKeyboardVolumeDelta);
      return true;
    }

    if (event == ftxui::Event::Character("+") || event == ftxui::Event::Character("="))
    {
      changeVolume(_playback, kKeyboardVolumeDelta);
      return true;
    }

    return false;
  }
} // namespace ao::tui
