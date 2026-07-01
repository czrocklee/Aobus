// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EventController.h"

#include "LibraryController.h"
#include "Model.h"
#include "OutputDeviceController.h"
#include "PlaybackActions.h"
#include "PlaybackPanel.h"
#include "Render.h"
#include "ShellModel.h"
#include "TrackTable.h"
#include <ao/rt/TrackField.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kPageSelectionDelta = 10;
    constexpr std::int32_t kMouseWheelSelectionDelta = 3;
    constexpr std::int32_t kBoundarySelectionDelta = 1'000'000;
    constexpr auto kKeyboardSeekDelta = std::chrono::seconds{5};
    constexpr float kKeyboardVolumeDelta = 0.05F;

    bool hasArea(ftxui::Box const& box)
    {
      return box.x_min <= box.x_max && box.y_min <= box.y_max &&
             (box.x_min != 0 || box.x_max != 0 || box.y_min != 0 || box.y_max != 0);
    }

    bool contains(ftxui::Box const& box, std::int32_t const column, std::int32_t const row)
    {
      return hasArea(box) && column >= box.x_min && column <= box.x_max && row >= box.y_min && row <= box.y_max;
    }

    bool containsTrackColumnResizeEdge(TrackColumnResizeHandle const& handle,
                                       std::int32_t const column,
                                       std::int32_t const row)
    {
      constexpr std::int32_t kResizeEdgeHitSlop = 1;

      return hasArea(handle.box) && row >= handle.box.y_min && row <= handle.box.y_max &&
             column >= handle.box.x_max - kResizeEdgeHitSlop && column <= handle.box.x_max + kResizeEdgeHitSlop;
    }

    bool containsTrackScrollbar(ftxui::Box const& tableBox, std::int32_t const column, std::int32_t const row)
    {
      constexpr std::int32_t kScrollbarHitSlop = 1;
      auto const bodyTop = tableBox.y_min + 1;

      return hasArea(tableBox) && column >= tableBox.x_max - kScrollbarHitSlop && column <= tableBox.x_max &&
             row >= bodyTop && row <= tableBox.y_max;
    }

    std::int32_t scrollbarTrackIndex(ftxui::Box const& tableBox,
                                     std::int32_t const row,
                                     std::size_t const trackCount,
                                     std::span<TrackSection const> const sections)
    {
      if (trackCount == 0)
      {
        return 0;
      }

      auto const bodyTop = tableBox.y_min + 1;
      auto const bodyBottom = tableBox.y_max;
      auto const bodyRows = std::max(1, bodyBottom - bodyTop + 1);
      auto const relativeRow = std::clamp(row - bodyTop, 0, bodyRows - 1);
      auto const maxVisualRows = std::min<std::size_t>(
        trackCount + sections.size() - 1, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()));
      auto const maxVisualRow = static_cast<std::int64_t>(maxVisualRows);
      auto const visualRow =
        bodyRows == 1
          ? std::int64_t{0}
          : ((static_cast<std::int64_t>(relativeRow) * maxVisualRow) + ((bodyRows - 1) / 2)) / (bodyRows - 1);
      return trackIndexForVisualRow(static_cast<std::int32_t>(visualRow), trackCount, sections);
    }

    void setColumnWidthOverride(std::vector<TrackColumnWidthOverride>& overrides,
                                rt::TrackField const field,
                                std::int32_t const columns)
    {
      auto const clampedColumns =
        std::clamp(columns, kMinimumTrackColumnWidthColumns, kMaximumTrackColumnResizeColumns);
      auto const it = std::ranges::find(overrides, field, &TrackColumnWidthOverride::field);

      if (it == overrides.end())
      {
        overrides.push_back(TrackColumnWidthOverride{.field = field, .columns = clampedColumns});
        return;
      }

      it->columns = clampedColumns;
    }

    bool overlayActive(Overlay const overlay) noexcept
    {
      return overlay != Overlay::None;
    }
  } // namespace

  EventController::EventController(ftxui::ScreenInteractive& screen,
                                   ShellModel& shell,
                                   LibraryController& library,
                                   rt::PlaybackService& playback,
                                   EventControllerBindings bindings)
    : _screen{screen}
    , _shell{shell}
    , _library{library}
    , _playback{playback}
    , _outputDevices{bindings.outputDevices}
    , _outputDeviceButtonBox{bindings.outputDeviceButtonBox}
    , _outputDeviceRowBoxes{bindings.outputDeviceRowBoxes}
    , _libraryButtonBox{bindings.libraryButtonBox}
    , _qualityButtonBox{bindings.qualityButtonBox}
    , _presentationButtonBox{bindings.presentationButtonBox}
    , _presentationRowBoxes{bindings.presentationRowBoxes}
    , _trackColumnResizeHandles{bindings.trackColumnResizeHandles}
    , _trackColumnWidthOverrides{bindings.trackColumnWidthOverrides}
    , _trackTableBox{bindings.trackTableBox}
    , _trackSectionRowBoxes{bindings.trackSectionRowBoxes}
    , _commandCompletionCallback{std::move(bindings.commandCompletionCallback)}
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

  void EventController::toggleListChooser()
  {
    if (_shell.overlay() == Overlay::ListChooser)
    {
      _shell.closeOverlay();
      _statusMessage = "Lists closed";
      return;
    }

    _shell.openOverlay(Overlay::ListChooser);
    _statusMessage = "Lists";
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

  void EventController::toggleOutputDevices()
  {
    if (_shell.overlay() == Overlay::OutputDevices)
    {
      _shell.closeOverlay();
      _statusMessage = "Output devices closed";
      return;
    }

    if (_outputDevices == nullptr)
    {
      _statusMessage = "Output devices unavailable";
      return;
    }

    _outputDevices->refresh();
    _shell.openOverlay(Overlay::OutputDevices);
    _statusMessage = "Output devices";
  }

  void EventController::togglePresentationPanel()
  {
    if (_shell.overlay() == Overlay::PresentationPanel)
    {
      _shell.closeOverlay();
      _statusMessage = "Views closed";
      return;
    }

    _shell.openOverlay(Overlay::PresentationPanel);
    _statusMessage = "Views";
  }

  void EventController::selectOutputDevice()
  {
    if (_outputDevices == nullptr)
    {
      _statusMessage = "Output devices unavailable";
      return;
    }

    _statusMessage = _outputDevices->selectSelected();
    _shell.closeOverlay();
  }

  void EventController::selectPresentation()
  {
    _statusMessage = _library.selectSelectedPresentation();
    _shell.closeOverlay();
  }

  void EventController::revealCurrentTrack()
  {
    _statusMessage = _library.revealTrack(_playback.state().trackId);
  }

  void EventController::runCommand(Command const& command)
  {
    switch (command.action)
    {
      case CommandAction::QuickFilter:
        _library.setFilterDraft(command.argument);
        applyFilter();
        break;
      case CommandAction::OpenLists: toggleListChooser(); break;
      case CommandAction::OpenDetail: toggleDetailPanel(); break;
      case CommandAction::OpenQuality: toggleQualityPanel(); break;
      case CommandAction::OpenOutputDevices: toggleOutputDevices(); break;
      case CommandAction::OpenPresentationPanel: togglePresentationPanel(); break;
      case CommandAction::CloseOverlay:
        _shell.closeOverlay();
        _statusMessage = "Overlay closed";
        break;
      case CommandAction::ShowHelp:
        _shell.openOverlay(Overlay::Help);
        _statusMessage = "Help";
        break;
      case CommandAction::RevealCurrentTrack: revealCurrentTrack(); break;
      case CommandAction::SetPresentation: _statusMessage = _library.setPresentation(command.argument); break;
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

  void EventController::refreshCommandCompletion()
  {
    if (!_shell.commandActive() || !_commandCompletionCallback)
    {
      _shell.clearCommandCompletion();
      return;
    }

    _shell.setCommandCompletion(_commandCompletionCallback(_shell.commandDraft()));
  }

  bool EventController::selectTrackFromScrollbar(std::int32_t const row)
  {
    if (_trackTableBox == nullptr || _library.tracks().empty())
    {
      return false;
    }

    auto const target = scrollbarTrackIndex(*_trackTableBox, row, _library.tracks().size(), _library.sections());
    _library.setSelectedTrackIndex(target);
    return true;
  }

  // TUI mouse dispatch is intentionally flat so hit-test ordering remains explicit.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  bool EventController::handleMouse(ftxui::Mouse const& mouse)
  {
    if (_optTrackScrollbarDrag)
    {
      if (mouse.motion == ftxui::Mouse::Moved || mouse.motion == ftxui::Mouse::Released)
      {
        auto const handled = selectTrackFromScrollbar(mouse.y);

        if (mouse.motion == ftxui::Mouse::Released)
        {
          _optTrackScrollbarDrag.reset();
        }

        return handled;
      }

      _optTrackScrollbarDrag.reset();
    }

    if (_optTrackColumnResizeDrag)
    {
      if (mouse.motion == ftxui::Mouse::Moved || mouse.motion == ftxui::Mouse::Released)
      {
        if (_trackColumnWidthOverrides != nullptr)
        {
          auto const columns = _optTrackColumnResizeDrag->startColumns + mouse.x - _optTrackColumnResizeDrag->startX;
          setColumnWidthOverride(*_trackColumnWidthOverrides, _optTrackColumnResizeDrag->field, columns);
          _statusMessage = "Column width updated";
        }

        if (mouse.motion == ftxui::Mouse::Released)
        {
          _optTrackColumnResizeDrag.reset();
        }

        return true;
      }

      _optTrackColumnResizeDrag.reset();
    }

    if ((mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) &&
        mouse.motion == ftxui::Mouse::Pressed)
    {
      if (_shell.overlay() == Overlay::None && _trackTableBox != nullptr && contains(*_trackTableBox, mouse.x, mouse.y))
      {
        auto const delta =
          mouse.button == ftxui::Mouse::WheelUp ? -kMouseWheelSelectionDelta : kMouseWheelSelectionDelta;
        _library.moveFocusedSelection(false, delta);
        return true;
      }

      return false;
    }

    if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Pressed)
    {
      return false;
    }

    if (_shell.overlay() == Overlay::None && _trackColumnResizeHandles != nullptr &&
        _trackColumnWidthOverrides != nullptr)
    {
      auto const handleIt = std::ranges::find_if(*_trackColumnResizeHandles,
                                                 [&](TrackColumnResizeHandle const& handle)
                                                 { return containsTrackColumnResizeEdge(handle, mouse.x, mouse.y); });

      if (handleIt != _trackColumnResizeHandles->end())
      {
        _optTrackColumnResizeDrag =
          TrackColumnResizeDrag{.field = handleIt->field, .startX = mouse.x, .startColumns = handleIt->columns};
        _statusMessage = "Resizing column";
        return true;
      }
    }

    if (_shell.overlay() == Overlay::None && _trackTableBox != nullptr &&
        containsTrackScrollbar(*_trackTableBox, mouse.x, mouse.y))
    {
      _optTrackScrollbarDrag = TrackScrollbarDrag{};

      if (!selectTrackFromScrollbar(mouse.y))
      {
        _optTrackScrollbarDrag.reset();
        return false;
      }

      return true;
    }

    if (_shell.overlay() == Overlay::None && _trackSectionRowBoxes != nullptr)
    {
      auto const rowBoxIt =
        std::ranges::find_if(*_trackSectionRowBoxes,
                             [&](TrackSectionRowBox const& rowBox) { return contains(rowBox.box, mouse.x, mouse.y); });

      if (rowBoxIt != _trackSectionRowBoxes->end())
      {
        if (rowBoxIt->sectionIndex < 0 ||
            static_cast<std::size_t>(rowBoxIt->sectionIndex) >= _library.sections().size())
        {
          _statusMessage = "Section is no longer available";
          return true;
        }

        _statusMessage = _library.selectSection(rowBoxIt->sectionIndex);
        return true;
      }
    }

    if (_outputDeviceButtonBox != nullptr && contains(*_outputDeviceButtonBox, mouse.x, mouse.y))
    {
      toggleOutputDevices();
      return true;
    }

    if (_qualityButtonBox != nullptr && contains(*_qualityButtonBox, mouse.x, mouse.y))
    {
      toggleQualityPanel();
      return true;
    }

    if (_libraryButtonBox != nullptr && contains(*_libraryButtonBox, mouse.x, mouse.y))
    {
      toggleListChooser();
      return true;
    }

    if (_presentationButtonBox != nullptr && contains(*_presentationButtonBox, mouse.x, mouse.y))
    {
      togglePresentationPanel();
      return true;
    }

    if (_shell.overlay() == Overlay::PresentationPanel && _presentationRowBoxes != nullptr)
    {
      auto const rowBoxIt =
        std::ranges::find_if(*_presentationRowBoxes,
                             [&](PresentationRowBox const& rowBox) { return contains(rowBox.box, mouse.x, mouse.y); });

      if (rowBoxIt != _presentationRowBoxes->end())
      {
        if (_library.setSelectedPresentation(rowBoxIt->rowIndex))
        {
          selectPresentation();
          return true;
        }
      }

      return false;
    }

    if (_shell.overlay() != Overlay::OutputDevices || _outputDevices == nullptr || _outputDeviceRowBoxes == nullptr)
    {
      return false;
    }

    auto const rowBoxIt = std::ranges::find_if(
      *_outputDeviceRowBoxes,
      [&](OutputDeviceRowBox const& rowBox)
      { return contains(rowBox.box, mouse.x, mouse.y) || contains(rowBox.secondaryBox, mouse.x, mouse.y); });

    if (rowBoxIt != _outputDeviceRowBoxes->end())
    {
      _statusMessage = _outputDevices->selectRow(rowBoxIt->rowIndex);
      _shell.closeOverlay();
      return true;
    }

    return false;
  }

  // TUI key dispatch is intentionally flat so shortcuts stay visible in one place.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  bool EventController::handleEvent(ftxui::Event const& event)
  {
    if (event.is_mouse())
    {
      auto mouseEvent = event;
      return handleMouse(mouseEvent.mouse());
    }

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

      if (event == ftxui::Event::Tab)
      {
        if (_shell.applyCommandCompletion())
        {
          refreshCommandCompletion();
          _statusMessage = "Command completed";
        }

        return true;
      }

      if (event == ftxui::Event::ArrowUp)
      {
        _shell.moveCommandCompletion(-1);
        return true;
      }

      if (event == ftxui::Event::ArrowDown)
      {
        _shell.moveCommandCompletion(1);
        return true;
      }

      if (event == ftxui::Event::Backspace)
      {
        _shell.backspaceCommand();
        refreshCommandCompletion();
        return true;
      }

      if (event.is_character())
      {
        _shell.appendCommandText(event.character());
        refreshCommandCompletion();
        return true;
      }

      return true;
    }

    if (event == ftxui::Event::CtrlC)
    {
      _playback.stop();
      _screen.ExitLoopClosure()();
      return true;
    }

    if (event == ftxui::Event::Escape)
    {
      _shell.closeOverlay();
      _statusMessage = "Overlay closed";
      return true;
    }

    if (overlayActive(_shell.overlay()))
    {
      switch (_shell.overlay())
      {
        case Overlay::ListChooser:
          if (event == ftxui::Event::Character("l"))
          {
            toggleListChooser();
            return true;
          }

          if (event == ftxui::Event::ArrowUp)
          {
            _library.moveFocusedSelection(true, -1);
            return true;
          }

          if (event == ftxui::Event::ArrowDown)
          {
            _library.moveFocusedSelection(true, 1);
            return true;
          }

          if (event == ftxui::Event::PageUp)
          {
            _library.moveFocusedSelection(true, -kPageSelectionDelta);
            return true;
          }

          if (event == ftxui::Event::PageDown)
          {
            _library.moveFocusedSelection(true, kPageSelectionDelta);
            return true;
          }

          if (event == ftxui::Event::Home)
          {
            _library.moveFocusedSelection(true, -kBoundarySelectionDelta);
            return true;
          }

          if (event == ftxui::Event::End)
          {
            _library.moveFocusedSelection(true, kBoundarySelectionDelta);
            return true;
          }

          if (event == ftxui::Event::Return)
          {
            openSelectedList();
            return true;
          }

          return true;
        case Overlay::DetailPanel:
          if (event == ftxui::Event::Character("d"))
          {
            toggleDetailPanel();
          }

          return true;
        case Overlay::QualityPanel:
          if (event == ftxui::Event::Character("a"))
          {
            toggleQualityPanel();
          }

          return true;
        case Overlay::OutputDevices:
          if (event == ftxui::Event::Character("o"))
          {
            toggleOutputDevices();
            return true;
          }

          if (event == ftxui::Event::ArrowUp && _outputDevices != nullptr)
          {
            _outputDevices->moveSelection(-1);
            return true;
          }

          if (event == ftxui::Event::ArrowDown && _outputDevices != nullptr)
          {
            _outputDevices->moveSelection(1);
            return true;
          }

          if (event == ftxui::Event::Return)
          {
            selectOutputDevice();
            return true;
          }

          return true;
        case Overlay::PresentationPanel:
          if (event == ftxui::Event::Character("v"))
          {
            togglePresentationPanel();
            return true;
          }

          if (event == ftxui::Event::ArrowUp)
          {
            _library.movePresentationSelection(-1);
            return true;
          }

          if (event == ftxui::Event::ArrowDown)
          {
            _library.movePresentationSelection(1);
            return true;
          }

          if (event == ftxui::Event::PageUp)
          {
            _library.movePresentationSelection(-kPageSelectionDelta);
            return true;
          }

          if (event == ftxui::Event::PageDown)
          {
            _library.movePresentationSelection(kPageSelectionDelta);
            return true;
          }

          if (event == ftxui::Event::Home)
          {
            _library.movePresentationSelection(-kBoundarySelectionDelta);
            return true;
          }

          if (event == ftxui::Event::End)
          {
            _library.movePresentationSelection(kBoundarySelectionDelta);
            return true;
          }

          if (event == ftxui::Event::Return)
          {
            selectPresentation();
            return true;
          }

          return true;
        case Overlay::Help: return true;
        case Overlay::None: break;
      }
    }

    if (event == ftxui::Event::Character("q"))
    {
      _playback.stop();
      _screen.ExitLoopClosure()();
      return true;
    }

    if (event == ftxui::Event::ArrowUp)
    {
      if (_shell.overlay() == Overlay::OutputDevices && _outputDevices != nullptr)
      {
        _outputDevices->moveSelection(-1);
        return true;
      }

      if (_shell.overlay() == Overlay::PresentationPanel)
      {
        _library.movePresentationSelection(-1);
        return true;
      }

      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, -1);
      return true;
    }

    if (event == ftxui::Event::ArrowDown)
    {
      if (_shell.overlay() == Overlay::OutputDevices && _outputDevices != nullptr)
      {
        _outputDevices->moveSelection(1);
        return true;
      }

      if (_shell.overlay() == Overlay::PresentationPanel)
      {
        _library.movePresentationSelection(1);
        return true;
      }

      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, 1);
      return true;
    }

    if (event == ftxui::Event::PageUp)
    {
      if (_shell.overlay() == Overlay::PresentationPanel)
      {
        _library.movePresentationSelection(-kPageSelectionDelta);
        return true;
      }

      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, -kPageSelectionDelta);
      return true;
    }

    if (event == ftxui::Event::PageDown)
    {
      if (_shell.overlay() == Overlay::PresentationPanel)
      {
        _library.movePresentationSelection(kPageSelectionDelta);
        return true;
      }

      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, kPageSelectionDelta);
      return true;
    }

    if (event == ftxui::Event::Home)
    {
      if (_shell.overlay() == Overlay::PresentationPanel)
      {
        _library.movePresentationSelection(-kBoundarySelectionDelta);
        return true;
      }

      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, -kBoundarySelectionDelta);
      return true;
    }

    if (event == ftxui::Event::End)
    {
      if (_shell.overlay() == Overlay::PresentationPanel)
      {
        _library.movePresentationSelection(kBoundarySelectionDelta);
        return true;
      }

      _library.moveFocusedSelection(_shell.overlay() == Overlay::ListChooser, kBoundarySelectionDelta);
      return true;
    }

    if (event == ftxui::Event::Character("/") || event == ftxui::Event::Character(":"))
    {
      _shell.beginCommand();
      refreshCommandCompletion();
      _statusMessage = "Command input";
      return true;
    }

    if (event == ftxui::Event::Character("l"))
    {
      toggleListChooser();
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

    if (event == ftxui::Event::Character("o"))
    {
      toggleOutputDevices();
      return true;
    }

    if (event == ftxui::Event::Character("v"))
    {
      togglePresentationPanel();
      return true;
    }

    if (event == ftxui::Event::Character("?"))
    {
      _shell.openOverlay(Overlay::Help);
      _statusMessage = "Help";
      return true;
    }

    if (event == ftxui::Event::CtrlL)
    {
      revealCurrentTrack();
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

      if (_shell.overlay() == Overlay::OutputDevices)
      {
        selectOutputDevice();
        return true;
      }

      if (_shell.overlay() == Overlay::PresentationPanel)
      {
        selectPresentation();
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

    if (event == ftxui::Event::Character("{") && _shell.overlay() == Overlay::None)
    {
      _statusMessage = _library.jumpToAdjacentSection(-1);
      return true;
    }

    if (event == ftxui::Event::Character("}") && _shell.overlay() == Overlay::None)
    {
      _statusMessage = _library.jumpToAdjacentSection(1);
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
