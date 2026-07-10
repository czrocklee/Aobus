// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EventController.h"

#include "LibraryController.h"
#include "ListNavigation.h"
#include "NotificationCenterPanel.h"
#include "OutputDeviceController.h"
#include "OutputDevicePanel.h"
#include "PlaybackActions.h"
#include "PlaybackPanel.h"
#include "PresentationPanel.h"
#include "ShellInteractionModel.h"
#include "TrackSection.h"
#include "TrackTable.h"
#include "TuiHitRegions.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/playback/seek/SeekSliderInteractionModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kMouseWheelSelectionDelta = 3;
    constexpr auto kKeyboardSeekDelta = std::chrono::seconds{5};
    constexpr float kKeyboardVolumeDelta = 0.05F;

    bool containsTrackColumnResizeEdge(TrackColumnResizeHandle const& handle,
                                       std::int32_t const column,
                                       std::int32_t const row)
    {
      constexpr std::int32_t kResizeEdgeHitSlop = 1;

      return hasHitArea(handle.box) && row >= handle.box.y_min && row <= handle.box.y_max &&
             column >= handle.box.x_max - kResizeEdgeHitSlop && column <= handle.box.x_max + kResizeEdgeHitSlop;
    }

    bool containsTrackScrollbar(ftxui::Box const& tableBox, std::int32_t const column, std::int32_t const row)
    {
      constexpr std::int32_t kScrollbarHitSlop = 1;
      auto const bodyTop = tableBox.y_min + 1;

      return hasHitArea(tableBox) && column >= tableBox.x_max - kScrollbarHitSlop && column <= tableBox.x_max &&
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

    bool isOverlayActive(Overlay const overlay) noexcept
    {
      return overlay != Overlay::None;
    }

    bool matchesOutputDeviceRow(uimodel::OutputDeviceRow const& row, OutputDeviceRowHitRegion const& hitRegion)
    {
      return row.kind == uimodel::OutputDeviceRow::Kind::DeviceProfile && row.backendId == hitRegion.backendId &&
             row.deviceId == hitRegion.deviceId && row.profileId == hitRegion.profileId;
    }
  } // namespace

  EventController::EventController(ftxui::ScreenInteractive& screen,
                                   ShellInteractionModel& shell,
                                   LibraryController& library,
                                   rt::AppRuntime& runtime,
                                   EventControllerBindings bindings)
    : _screen{screen}
    , _shell{shell}
    , _library{library}
    , _playback{runtime.playback()}
    , _playbackQueue{runtime.playbackQueue()}
    , _outputDevices{bindings.outputDevices}
    , _hitRegions{bindings.hitRegions}
    , _trackColumnWidthOverrides{bindings.trackColumnWidthOverrides}
    , _activityStatusViewModel{bindings.activityStatusViewModel}
    , _notifications{bindings.notifications}
    , _commandCompletionCallback{std::move(bindings.commandCompletionCallback)}
  {
  }

  void EventController::postActivityNotification(rt::NotificationSeverity const severity, std::string message)
  {
    if (_notifications != nullptr)
    {
      _notifications->post(rt::NotificationRequest{.severity = severity, .message = std::move(message)});
    }
  }

  void EventController::openSelectedList()
  {
    if (auto const result = _library.openSelectedList(); result.opened)
    {
      _shell.closeOverlay();
    }
  }

  void EventController::reloadActiveList()
  {
    _library.reloadActiveList();
  }

  void EventController::applyFilter()
  {
    _library.applyFilter();
  }

  void EventController::toggleListChooser()
  {
    if (_shell.overlay() == Overlay::ListChooser)
    {
      _shell.closeOverlay();
      postActivityNotification(rt::NotificationSeverity::Info, "Lists closed");
      return;
    }

    _shell.openOverlay(Overlay::ListChooser);
    postActivityNotification(rt::NotificationSeverity::Info, "Lists");
  }

  void EventController::toggleDetailPanel()
  {
    if (_shell.overlay() == Overlay::DetailPanel)
    {
      _shell.closeOverlay();
      postActivityNotification(rt::NotificationSeverity::Info, "Detail closed");
      return;
    }

    _shell.openOverlay(Overlay::DetailPanel);
    postActivityNotification(rt::NotificationSeverity::Info, "Detail panel");
  }

  void EventController::toggleQualityPanel()
  {
    if (_shell.overlay() == Overlay::QualityPanel)
    {
      _shell.closeOverlay();
      postActivityNotification(rt::NotificationSeverity::Info, "Pipeline closed");
      return;
    }

    _shell.openOverlay(Overlay::QualityPanel);
    postActivityNotification(rt::NotificationSeverity::Info, "Audio pipeline");
  }

  void EventController::toggleOutputDevices()
  {
    if (_shell.overlay() == Overlay::OutputDevices)
    {
      _shell.closeOverlay();
      postActivityNotification(rt::NotificationSeverity::Info, "Output devices closed");
      return;
    }

    if (_outputDevices == nullptr)
    {
      postActivityNotification(rt::NotificationSeverity::Warning, "Output devices unavailable");
      return;
    }

    _outputDevices->refresh();
    _shell.openOverlay(Overlay::OutputDevices);
    postActivityNotification(rt::NotificationSeverity::Info, "Output devices");
  }

  void EventController::togglePresentationPanel()
  {
    if (_shell.overlay() == Overlay::PresentationPanel)
    {
      _shell.closeOverlay();
      postActivityNotification(rt::NotificationSeverity::Info, "Views closed");
      return;
    }

    _shell.openOverlay(Overlay::PresentationPanel);
    postActivityNotification(rt::NotificationSeverity::Info, "Views");
  }

  void EventController::toggleNotificationCenter()
  {
    if (_shell.overlay() == Overlay::Notifications)
    {
      _shell.closeOverlay();
      postActivityNotification(rt::NotificationSeverity::Info, "Notifications closed");
      return;
    }

    if (_activityStatusViewModel == nullptr)
    {
      return;
    }

    if (auto const& view = _activityStatusViewModel->viewState();
        view.compact.kind == uimodel::ActivityStatusKind::Idle && !uimodel::hasDetailContent(view.detail))
    {
      return;
    }

    _shell.openOverlay(Overlay::Notifications);
    postActivityNotification(rt::NotificationSeverity::Info, "Notifications");
  }

  void EventController::selectOutputDevice()
  {
    if (_outputDevices == nullptr)
    {
      postActivityNotification(rt::NotificationSeverity::Warning, "Output devices unavailable");
      return;
    }

    _outputDevices->selectSelected();
    _shell.closeOverlay();
  }

  void EventController::selectPresentation()
  {
    _library.selectSelectedPresentation();
    _shell.closeOverlay();
  }

  void EventController::revealCurrentTrack()
  {
    _library.revealTrack(_playback.state().nowPlaying.trackId);
  }

  void EventController::togglePlaybackFromSelection()
  {
    if (!togglePlayback(
          _playback, _playbackQueue, _library.tracks(), _library.selectedTrack(), _library.currentListId()))
    {
      postActivityNotification(
        rt::NotificationSeverity::Warning, "Playback did not start. Check output device, file path, and logs.");
    }
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
      case CommandAction::OpenNotifications: toggleNotificationCenter(); break;
      case CommandAction::CloseOverlay:
        _shell.closeOverlay();
        postActivityNotification(rt::NotificationSeverity::Info, "Overlay closed");
        break;
      case CommandAction::ShowHelp:
        _shell.openOverlay(Overlay::Help);
        postActivityNotification(rt::NotificationSeverity::Info, "Help");
        break;
      case CommandAction::RevealCurrentTrack: revealCurrentTrack(); break;
      case CommandAction::SetPresentation: _library.setPresentation(command.argument); break;
      case CommandAction::ClearFilter:
        _library.clearFilterDraft();
        applyFilter();
        break;
      case CommandAction::Reload: reloadActiveList(); break;
      case CommandAction::Play:
        if (!playSelected(_playbackQueue, _library.tracks(), _library.selectedTrack(), _library.currentListId()))
        {
          postActivityNotification(
            rt::NotificationSeverity::Warning, "Playback did not start. Check output device, file path, and logs.");
        }

        break;
      case CommandAction::TogglePlayback: togglePlaybackFromSelection(); break;
      case CommandAction::Stop: _playback.stop(); break;
      case CommandAction::Quit:
        _playback.stop();
        _screen.ExitLoopClosure()();
        break;
    }
  }

  void EventController::refreshCommandCompletion()
  {
    if (!_shell.isCommandActive() || !_commandCompletionCallback)
    {
      _shell.clearCommandCompletion();
      return;
    }

    _shell.setCommandCompletion(_commandCompletionCallback(_shell.commandDraft()));
  }

  bool EventController::selectTrackFromScrollbar(std::int32_t const row)
  {
    if (_hitRegions == nullptr || _library.tracks().empty())
    {
      return false;
    }

    auto const target =
      scrollbarTrackIndex(_hitRegions->trackTableBox, row, _library.tracks().size(), _library.sections());
    _library.setSelectedTrackIndex(target);
    return true;
  }

  void EventController::syncSeekSlider()
  {
    auto const duration = _playback.state().duration;
    _seekSlider.applyViewState(duration, duration > std::chrono::milliseconds{0});
  }

  std::chrono::milliseconds EventController::seekRailElapsed(std::int32_t const column) const
  {
    if (_hitRegions == nullptr)
    {
      return std::chrono::milliseconds{0};
    }

    auto const duration = _playback.state().duration;

    if (duration <= std::chrono::milliseconds{0})
    {
      return std::chrono::milliseconds{0};
    }

    auto const& seekRailBox = _hitRegions->seekRailBox;
    auto const railColumns = std::max(1, seekRailBox.x_max - seekRailBox.x_min + 1);
    auto const denominator = std::max(1, railColumns - 1);
    auto const relativeColumn = std::clamp(column - seekRailBox.x_min, 0, denominator);
    auto const fraction = static_cast<double>(relativeColumn) / static_cast<double>(denominator);
    auto const elapsed =
      static_cast<std::chrono::milliseconds::rep>(std::llround(static_cast<double>(duration.count()) * fraction));
    return std::chrono::milliseconds{elapsed};
  }

  void EventController::applySeekDecision(uimodel::SeekSliderDecision const& decision)
  {
    switch (decision.action)
    {
      case uimodel::SeekSliderAction::None: return;
      case uimodel::SeekSliderAction::Preview:
        _playback.seek(decision.elapsed, rt::PlaybackService::SeekMode::Preview);
        return;
      case uimodel::SeekSliderAction::Commit: _playback.seek(decision.elapsed); return;
    }
  }

  void EventController::cancelSeekInteraction()
  {
    if (!_optSeekRailDrag)
    {
      return;
    }

    if (_seekSlider.hasPendingFinalSeek())
    {
      _playback.seek(_playback.state().elapsed);
    }

    _optSeekRailDrag.reset();
    _seekSlider.reset();
  }

  // TUI mouse dispatch is intentionally flat so hit-test ordering remains explicit.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  bool EventController::handleMouse(ftxui::Mouse const& mouse)
  {
    auto const modalInputActive = _shell.isCommandActive() || isOverlayActive(_shell.overlay());

    if (modalInputActive && _optSeekRailDrag)
    {
      cancelSeekInteraction();
      return false;
    }

    if (_shell.isCommandActive())
    {
      _optTrackScrollbarDrag.reset();
      _optTrackColumnResizeDrag.reset();

      if (mouse.motion != ftxui::Mouse::Moved)
      {
        return false;
      }
    }

    if (_optSeekRailDrag)
    {
      if (mouse.motion == ftxui::Mouse::Moved || mouse.motion == ftxui::Mouse::Released)
      {
        syncSeekSlider();
        auto const elapsed = seekRailElapsed(mouse.x);
        auto const decision = mouse.motion == ftxui::Mouse::Released ? _seekSlider.endPointerInteraction(elapsed)
                                                                     : _seekSlider.valueChanged(elapsed);
        applySeekDecision(decision);

        if (mouse.motion == ftxui::Mouse::Released)
        {
          _optSeekRailDrag.reset();
        }

        return true;
      }

      _optSeekRailDrag.reset();
      _seekSlider.reset();
    }

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
      if (_shell.overlay() == Overlay::None && _hitRegions != nullptr &&
          contains(_hitRegions->trackTableBox, mouse.x, mouse.y))
      {
        auto const delta =
          mouse.button == ftxui::Mouse::WheelUp ? -kMouseWheelSelectionDelta : kMouseWheelSelectionDelta;
        _library.moveFocusedSelection(false, delta);
        return true;
      }

      return false;
    }

    if (mouse.motion == ftxui::Mouse::Moved)
    {
      auto const buttonHit =
        _hitRegions == nullptr
          ? ButtonHitTestResult{}
          : _hitRegions->hitTestButton(mouse.x,
                                       mouse.y,
                                       HitTestContext{.isCommandActive = _shell.isCommandActive(),
                                                      .isOverlayActive = isOverlayActive(_shell.overlay())});
      bool handled = false;

      if (_hoveredButton != buttonHit.hoveredButton)
      {
        _hoveredButton = buttonHit.hoveredButton;
        handled = true;
      }

      if (_qualityHoverVisible != buttonHit.isQualityHoverVisible)
      {
        _qualityHoverVisible = buttonHit.isQualityHoverVisible;
        handled = true;
      }

      return handled;
    }

    if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Pressed)
    {
      return false;
    }

    if (!modalInputActive && _hitRegions != nullptr && contains(_hitRegions->seekRailBox, mouse.x, mouse.y))
    {
      syncSeekSlider();

      if (!_seekSlider.beginPointerInteraction())
      {
        return false;
      }

      _optSeekRailDrag = SeekRailDrag{};
      applySeekDecision(_seekSlider.valueChanged(seekRailElapsed(mouse.x)));
      return true;
    }

    if (_shell.overlay() == Overlay::None && _hitRegions != nullptr && _trackColumnWidthOverrides != nullptr)
    {
      auto const handleIt = std::ranges::find_if(_hitRegions->trackColumnResizeHandles,
                                                 [&](TrackColumnResizeHandle const& handle)
                                                 { return containsTrackColumnResizeEdge(handle, mouse.x, mouse.y); });

      if (handleIt != _hitRegions->trackColumnResizeHandles.end())
      {
        _optTrackColumnResizeDrag =
          TrackColumnResizeDrag{.field = handleIt->field, .startX = mouse.x, .startColumns = handleIt->columns};
        return true;
      }
    }

    if (_shell.overlay() == Overlay::None && _hitRegions != nullptr &&
        containsTrackScrollbar(_hitRegions->trackTableBox, mouse.x, mouse.y))
    {
      _optTrackScrollbarDrag = TrackScrollbarDrag{};

      if (!selectTrackFromScrollbar(mouse.y))
      {
        _optTrackScrollbarDrag.reset();
        return false;
      }

      return true;
    }

    if (_shell.overlay() == Overlay::None && _hitRegions != nullptr)
    {
      auto const hitRegionIt = std::ranges::find_if(_hitRegions->trackSectionRows,
                                                    [&](TrackSectionRowHitRegion const& hitRegion)
                                                    { return contains(hitRegion.box, mouse.x, mouse.y); });

      if (hitRegionIt != _hitRegions->trackSectionRows.end())
      {
        if (hitRegionIt->sectionIndex < 0 ||
            static_cast<std::size_t>(hitRegionIt->sectionIndex) >= _library.sections().size())
        {
          postActivityNotification(rt::NotificationSeverity::Warning, "Section is no longer available");
          return true;
        }

        _library.selectSection(hitRegionIt->sectionIndex);
        return true;
      }
    }

    if (_hitRegions != nullptr && contains(_hitRegions->outputDeviceButtonBox, mouse.x, mouse.y))
    {
      toggleOutputDevices();
      return true;
    }

    if (_hitRegions != nullptr && contains(_hitRegions->soulButtonBox, mouse.x, mouse.y))
    {
      togglePlaybackFromSelection();
      return true;
    }

    if (_hitRegions != nullptr && contains(_hitRegions->libraryButtonBox, mouse.x, mouse.y))
    {
      toggleListChooser();
      return true;
    }

    if (_hitRegions != nullptr && contains(_hitRegions->presentationButtonBox, mouse.x, mouse.y))
    {
      togglePresentationPanel();
      return true;
    }

    if (_hitRegions != nullptr && contains(_hitRegions->activityStatusBox, mouse.x, mouse.y))
    {
      if (_activityStatusViewModel == nullptr)
      {
        return true;
      }

      auto const& view = _activityStatusViewModel->viewState();

      if (uimodel::hasDetailContent(view.detail) || view.compact.hasDetails)
      {
        toggleNotificationCenter();
        return true;
      }

      if (view.compact.dismissible)
      {
        _activityStatusViewModel->dismissCompact();
        return true;
      }

      return true;
    }

    if (_shell.overlay() == Overlay::PresentationPanel && _hitRegions != nullptr)
    {
      auto const hitRegionIt = std::ranges::find_if(_hitRegions->presentationRows,
                                                    [&](PresentationRowHitRegion const& hitRegion)
                                                    { return contains(hitRegion.box, mouse.x, mouse.y); });

      if (hitRegionIt != _hitRegions->presentationRows.end())
      {
        if (_library.setSelectedPresentation(hitRegionIt->rowIndex))
        {
          selectPresentation();
          return true;
        }
      }

      return false;
    }

    if (_shell.overlay() == Overlay::Notifications && _activityStatusViewModel != nullptr && _hitRegions != nullptr)
    {
      auto const hitRegionIt = std::ranges::find_if(_hitRegions->notificationDetailRows,
                                                    [&](NotificationDetailRowHitRegion const& hitRegion)
                                                    { return contains(hitRegion.box, mouse.x, mouse.y); });

      if (hitRegionIt != _hitRegions->notificationDetailRows.end())
      {
        if (hitRegionIt->dismissible)
        {
          _activityStatusViewModel->dismissDetailNotificationFromActivity(hitRegionIt->id);
          return true;
        }

        return true;
      }

      return false;
    }

    if (_shell.overlay() != Overlay::OutputDevices || _outputDevices == nullptr || _hitRegions == nullptr)
    {
      return false;
    }

    auto const hitRegionIt = std::ranges::find_if(
      _hitRegions->outputDeviceRows,
      [&](OutputDeviceRowHitRegion const& hitRegion)
      { return contains(hitRegion.box, mouse.x, mouse.y) || contains(hitRegion.secondaryBox, mouse.x, mouse.y); });

    if (hitRegionIt != _hitRegions->outputDeviceRows.end())
    {
      if (hitRegionIt->rowIndex < 0 ||
          static_cast<std::size_t>(hitRegionIt->rowIndex) >= _outputDevices->viewState().rows.size())
      {
        return true;
      }

      auto const& row = _outputDevices->viewState().rows[static_cast<std::size_t>(hitRegionIt->rowIndex)];

      if (!matchesOutputDeviceRow(row, *hitRegionIt))
      {
        return true;
      }

      if (_outputDevices->selectRow(hitRegionIt->rowIndex))
      {
        _shell.closeOverlay();
      }

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

    if (_shell.isCommandActive())
    {
      if (event == ftxui::Event::Escape)
      {
        _shell.cancelCommand();
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
      postActivityNotification(rt::NotificationSeverity::Info, "Overlay closed");
      return true;
    }

    if (isOverlayActive(_shell.overlay()))
    {
      switch (_shell.overlay())
      {
        case Overlay::ListChooser:
          if (event == ftxui::Event::Character("l"))
          {
            toggleListChooser();
            return true;
          }

          if (handleListNavigation(
                event, [this](std::int32_t const delta) { _library.moveFocusedSelection(true, delta); }))
          {
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

          if (_outputDevices != nullptr &&
              handleListNavigation(event, [this](std::int32_t const delta) { _outputDevices->moveSelection(delta); }))
          {
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

          if (handleListNavigation(
                event, [this](std::int32_t const delta) { _library.movePresentationSelection(delta); }))
          {
            return true;
          }

          if (event == ftxui::Event::Return)
          {
            selectPresentation();
            return true;
          }

          return true;
        case Overlay::Notifications:
          if (event == ftxui::Event::Character("n"))
          {
            toggleNotificationCenter();
            return true;
          }

          if (event == ftxui::Event::Character("x") && _activityStatusViewModel != nullptr &&
              _activityStatusViewModel->viewState().compact.dismissible)
          {
            _activityStatusViewModel->dismissCompact();
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

    if (handleListNavigation(event, [this](std::int32_t const delta) { _library.moveFocusedSelection(false, delta); }))
    {
      return true;
    }

    if (event == ftxui::Event::Character("/") || event == ftxui::Event::Character(":"))
    {
      _shell.beginCommand();
      refreshCommandCompletion();
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

    if (event == ftxui::Event::Character("n"))
    {
      toggleNotificationCenter();
      return true;
    }

    if (event == ftxui::Event::Character("?"))
    {
      _shell.openOverlay(Overlay::Help);
      postActivityNotification(rt::NotificationSeverity::Info, "Help");
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
      if (!playSelected(_playbackQueue, _library.tracks(), _library.selectedTrack(), _library.currentListId()))
      {
        postActivityNotification(
          rt::NotificationSeverity::Warning, "Playback did not start. Check output device, file path, and logs.");
      }

      return true;
    }

    if (event == ftxui::Event::Character("p"))
    {
      if (!playSelected(_playbackQueue, _library.tracks(), _library.selectedTrack(), _library.currentListId()))
      {
        postActivityNotification(
          rt::NotificationSeverity::Warning, "Playback did not start. Check output device, file path, and logs.");
      }

      return true;
    }

    if (event == ftxui::Event::Character(" "))
    {
      togglePlaybackFromSelection();
      return true;
    }

    if (event == ftxui::Event::Character("s"))
    {
      _playback.stop();
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
      _library.jumpToAdjacentSection(-1);
      return true;
    }

    if (event == ftxui::Event::Character("}") && _shell.overlay() == Overlay::None)
    {
      _library.jumpToAdjacentSection(1);
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
