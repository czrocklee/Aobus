// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EventController.h"

#include "Command.h"
#include "HitRegions.h"
#include "Keymap.h"
#include "LibraryController.h"
#include "LibraryScanController.h"
#include "MouseBindings.h"
#include "NotificationCenterPanel.h"
#include "OutputDeviceController.h"
#include "OutputDevicePanel.h"
#include "PlaybackPanel.h"
#include "PresentationPanel.h"
#include "SelectionNavigation.h"
#include "SettingsEditor.h"
#include "ShellInteractionModel.h"
#include "TerminalTrackColumnLayout.h"
#include "TrackEditController.h"
#include "TrackListEntry.h"
#include "TrackSection.h"
#include "TrackTable.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>
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
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr auto kFilterDebounceInterval = std::chrono::milliseconds{200};

    bool isPopoverOverlay(Overlay const overlay)
    {
      switch (overlay)
      {
        case Overlay::QualityPanel:
        case Overlay::OutputDevices:
        case Overlay::PresentationPanel:
        case Overlay::Notifications:
        case Overlay::Help: return true;
        case Overlay::None:
        case Overlay::DetailPanel: return false;
      }

      return false;
    }

    bool tryPlaySelected(rt::PlaybackCommands& commands,
                         std::vector<TrackListEntry> const& tracks,
                         std::int32_t const selected,
                         rt::ViewId const sourceViewId)
    {
      if (tracks.empty())
      {
        return false;
      }

      auto const index = clampSelection(static_cast<std::size_t>(std::max(0, selected)), tracks.size());
      return static_cast<bool>(commands.startFromView(sourceViewId, tracks[index].id));
    }

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

    /**
     * @brief Whether any overlay currently occupies the screen.
     *
     * Visibility, not modality: a visible overlay owns its own keys and its own
     * share of the layout even when the workspace beneath it stays live. Use
     * @ref isModalOverlay to ask whether the workspace may still be driven.
     */
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

  EventController::EventController(ShellInteractionModel& shell,
                                   LibraryController& library,
                                   async::Runtime& asyncRuntime,
                                   rt::PlaybackService& playback,
                                   KeymapPlan const& keymapPlan,
                                   EventControllerBindings bindings)
    : _shell{shell}
    , _library{library}
    , _keymapPlan{keymapPlan}
    , _asyncRuntime{asyncRuntime}
    , _playback{playback}
    , _playbackActions{_playback, [this] { playSelectedTrack(); }}
    , _seekViewModel{_playback, {}}
    , _volumeViewModel{_playback}
    , _outputDevices{bindings.outputDevices}
    , _hitRegions{bindings.hitRegions}
    , _trackColumnLayouts{bindings.trackColumnLayouts}
    , _trackColumnResizePreview{bindings.trackColumnResizePreview}
    , _activityStatusViewModel{bindings.activityStatusViewModel}
    , _notifications{bindings.notifications}
    , _libraryScan{bindings.libraryScan}
    , _trackEdit{bindings.trackEdit}
    , _settings{bindings.settings}
    , _preferences{bindings.preferences}
    , _requestExit{std::move(bindings.requestExit)}
    , _isExitWaiting{std::move(bindings.isExitWaiting)}
    , _commandCompletionCallback{std::move(bindings.commandCompletionCallback)}
    , _filterCompletionCallback{std::move(bindings.filterCompletionCallback)}
    , _requestLayoutCheckpoint{std::move(bindings.requestLayoutCheckpoint)}
  {
    _revealSub = _playback.events().onRevealTrackRequested(
      [this](rt::PlaybackRevealTrackRequest const& request)
      {
        postActivityNotification(
          rt::NotificationSeverity::Info,
          _library.revealTrack(request.trackId, request.preferredViewId, request.preferredListId));

        if (request.trackId != kInvalidTrackId && _library.selectedTrackView().track != nullptr &&
            _library.selectedTrackView().track->id == request.trackId)
        {
          leaveNavigation();
        }
      });
  }

  bool EventController::tryHandleEvent(ftxui::Event const& event)
  {
    syncWorkspaceGeometry();

    if (event == ftxui::Event::CtrlC)
    {
      _requestExit();
      return true;
    }

    if (event == ftxui::Event::Custom)
    {
      return false;
    }

    if (!event.is_mouse())
    {
      _lastClickedTrack = kInvalidTrackId;
    }

    // Waiting for a submitted write swallows ordinary input; only the Ctrl-C
    // above can stop the wait.
    if (_isExitWaiting && _isExitWaiting())
    {
      return true;
    }

    if (event.is_mouse() && !_preferences.mouseEnabled)
    {
      return true;
    }

    // An open editor owns the whole surface, including keys and mouse events
    // it has no use for, so nothing behind it can act on stale geometry.
    if (_settings.tryHandleEvent(event) || _trackEdit.tryHandleEvent(event))
    {
      return true;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;

      if (tryHandleListSearchEvent(event))
      {
        return true;
      }

      return tryHandleMouse(mouseEvent.mouse());
    }

    if (_shell.isInputActive())
    {
      return tryHandleCommandEvent(event);
    }

    if (tryHandleListSearchEvent(event))
    {
      return true;
    }

    if (tryHandleNavigationEvent(event))
    {
      return true;
    }

    // Escape is protocol-owned before any overlay sees it, so rebinding cannot
    // strand the user inside a modal surface. A running visual selection is the
    // innermost transient state of the workspace, so it wins whenever the
    // workspace is still drivable; a modal overlay took the keys that grow the
    // selection, so it is closed first and the selection survives.
    if (event == ftxui::Event::Escape)
    {
      if (!isModalOverlay(_shell.overlay()) && _library.isVisualSelectionActive())
      {
        _library.cancelVisualSelection();
        return true;
      }

      runCommand({.action = CommandAction::CloseOverlay});
      return true;
    }

    // A modal overlay answers everything, so reaching the workspace below means
    // the open overlay left this key alone.
    if (isOverlayActive(_shell.overlay()) && tryHandleOverlayEvent(event))
    {
      return true;
    }

    return tryHandleRootEvent(event);
  }

  void EventController::cancelTransientInteractions()
  {
    _lastClickedTrack = kInvalidTrackId;
    _navigationScrollbarDrag = false;
    cancelFilterDebounce();
    cancelWorkspaceGestures();
    _qualityHoverVisible = false;
    _hoveredButton = HoveredButton::None;
  }

  void EventController::reloadActiveList()
  {
    _library.reloadActiveList();
  }

  void EventController::applyFilter(bool const reportError)
  {
    if (auto res = _library.applyFilter(); !res)
    {
      APP_LOG_ERROR("Failed to apply TUI filter: {}", res.error().message);

      if (reportError)
      {
        postActivityNotification(
          rt::NotificationSeverity::Error,
          i18n::requiredFormat(
            _library.textCatalog(), i18n::MessageId::TuiFilterFailed, {{"detail", res.error().message}}));
      }

      return;
    }

    if (reportError && !_library.filterError().empty())
    {
      postActivityNotification(rt::NotificationSeverity::Warning, _library.filterError());
    }
  }

  void EventController::toggleDetailPanel()
  {
    if (_shell.overlay() == Overlay::DetailPanel)
    {
      closeOverlay();
      postActivityNotification(
        rt::NotificationSeverity::Info,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiDetailClosed)});
      return;
    }

    openOverlay(Overlay::DetailPanel);
    postActivityNotification(rt::NotificationSeverity::Info,
                             std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiDetailOpened)});
  }

  void EventController::toggleQualityPanel()
  {
    if (_shell.overlay() == Overlay::QualityPanel)
    {
      closeOverlay();
      postActivityNotification(
        rt::NotificationSeverity::Info,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiPipelineClosed)});
      return;
    }

    openOverlay(Overlay::QualityPanel);
    postActivityNotification(
      rt::NotificationSeverity::Info,
      std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiPipelineOpened)});
  }

  void EventController::toggleOutputDevices()
  {
    if (_shell.overlay() == Overlay::OutputDevices)
    {
      closeOverlay();
      postActivityNotification(
        rt::NotificationSeverity::Info,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiOutputClosed)});
      return;
    }

    _outputDevices.refresh();
    openOverlay(Overlay::OutputDevices);
    postActivityNotification(rt::NotificationSeverity::Info,
                             std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiOutputOpened)});
  }

  void EventController::togglePresentationPanel()
  {
    if (_shell.overlay() == Overlay::PresentationPanel)
    {
      closeOverlay();
      postActivityNotification(
        rt::NotificationSeverity::Info,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiViewsClosed)});
      return;
    }

    openOverlay(Overlay::PresentationPanel);
    postActivityNotification(rt::NotificationSeverity::Info,
                             std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiViewsOpened)});
  }

  void EventController::toggleNotificationCenter()
  {
    if (_shell.overlay() == Overlay::Notifications)
    {
      closeOverlay();
      postActivityNotification(
        rt::NotificationSeverity::Info,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiNotificationsClosed)});
      return;
    }

    if (auto const& view = _activityStatusViewModel.viewState();
        view.compact.kind == uimodel::ActivityStatusKind::Idle && !uimodel::hasDetailContent(view.detail))
    {
      return;
    }

    openOverlay(Overlay::Notifications);
    postActivityNotification(
      rt::NotificationSeverity::Info,
      std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiNotificationsOpened)});
  }

  void EventController::editSelectedTrackProperties()
  {
    if (!_trackEdit.tryOpen(_library.selectedTrackIds()))
    {
      return;
    }

    // The editor takes the whole surface, so whatever the workspace was in the
    // middle of ends here instead of finishing against a layout nobody can see.
    cancelTransientInteractions();
    // The editor captured the range the moment it opened. Leaving the range
    // armed would let the next motion key after the modal closes reshape the
    // marks the user comes back to, and a library change under the modal would
    // re-derive it against rows the edit itself reordered.
    _library.commitVisualSelection();
    _shell.closeInput();
  }

  void EventController::selectOutputDevice()
  {
    _outputDevices.trySelectSelected();
    closeOverlay();
  }

  void EventController::selectPresentation()
  {
    _library.selectSelectedPresentation();
    closeOverlay();
  }

  void EventController::revealCurrentTrack()
  {
    _playback.commands().revealPlayingTrack();
  }

  void EventController::playSelectedTrack()
  {
    if (!tryPlaySelected(_playback.commands(), _library.tracks(), _library.selectedTrack(), _library.activeViewId()))
    {
      postActivityNotification(
        rt::NotificationSeverity::Warning,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiPlaybackStartFailed)});
    }
  }

  void EventController::executePlaybackCommand(uimodel::PlaybackCommand const command)
  {
    if (!_playbackActions.tryExecute(command))
    {
      if (command != uimodel::PlaybackCommand::Stop)
      {
        postActivityNotification(
          rt::NotificationSeverity::Warning,
          std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiPlaybackControlUnavailable)});
      }

      return;
    }

    if (command == uimodel::PlaybackCommand::ToggleShuffle || command == uimodel::PlaybackCommand::CycleRepeat)
    {
      auto const& state = _playback.snapshot().succession;
      auto repeat = std::string_view{"off"};

      switch (state.repeat)
      {
        case rt::RepeatMode::Off: break;
        case rt::RepeatMode::All: repeat = "all"; break;
        case rt::RepeatMode::One: repeat = "one"; break;
      }

      postActivityNotification(
        rt::NotificationSeverity::Info,
        i18n::requiredFormat(_library.textCatalog(),
                             i18n::MessageId::TuiPlaybackModes,
                             {{"shuffle", state.shuffle == rt::ShuffleMode::On ? "on" : "off"}, {"repeat", repeat}}));
    }
  }

  void EventController::executeKeyAction(KeyAction const action)
  {
    if (auto const optCommandAction = commandActionForKeyAction(action); optCommandAction)
    {
      runCommand(Command{.action = *optCommandAction});
      return;
    }

    using enum KeyAction;

    switch (action)
    {
      case SwitchWorkspaceFocus: switchWorkspaceFocus(); break;
      case OpenCommandPalette:
      case OpenQuickFilter:
        if (action == OpenQuickFilter)
        {
          leaveNavigation();
        }

        cancelWorkspaceGestures();
        _shell.beginInput(action == OpenQuickFilter ? ShellInputMode::QuickFilter : ShellInputMode::Command);
        refreshCommandCompletion();
        break;
      case PreviousRow: _library.moveTrackSelection(-1); break;
      case NextRow: _library.moveTrackSelection(1); break;
      case PreviousSection: _library.jumpToAdjacentSection(-1); break;
      case NextSection: _library.jumpToAdjacentSection(1); break;
      case SeekBackward: _seekViewModel.seekBy(-std::chrono::seconds{_preferences.seekSeconds}); break;
      case SeekForward: _seekViewModel.seekBy(std::chrono::seconds{_preferences.seekSeconds}); break;
      case VolumeDown: _volumeViewModel.adjustVolume(-static_cast<float>(_preferences.volumePercent) / 100.0F); break;
      case VolumeUp: _volumeViewModel.adjustVolume(static_cast<float>(_preferences.volumePercent) / 100.0F); break;
      case Quit:
      case ToggleLists:
      case ToggleDetails:
      case ToggleAudioPipeline:
      case ToggleOutputDevices:
      case TogglePresentations:
      case ToggleNotifications:
      case ShowHelp:
      case RevealCurrentTrack:
      case ClearFilter:
      case Reload:
      case Scan:
      case ScanCancel:
      case SelectToggle:
      case SelectVisual:
      case SelectAll:
      case SelectClear:
      case EditProperties:
      case OpenSettings:
      case PlaySelection:
      case PlaybackPlayPause:
      case PlaybackPrevious:
      case PlaybackNext:
      case PlaybackShuffle:
      case PlaybackRepeat:
      case PlaybackStop: AO_FATAL("Command-backed TUI key action was not mapped");
      case Count: break;
    }
  }

  void EventController::runCommand(Command const& command)
  {
    switch (command.action)
    {
      case CommandAction::QuickFilter:
        leaveNavigation();
        _library.setFilterDraft(command.argument);
        applyFilter();
        break;
      case CommandAction::OpenLists: toggleLists(); break;
      case CommandAction::OpenDetail: toggleDetailPanel(); break;
      case CommandAction::OpenQuality: toggleQualityPanel(); break;
      case CommandAction::OpenOutputDevices: toggleOutputDevices(); break;
      case CommandAction::OpenPresentationPanel: togglePresentationPanel(); break;
      case CommandAction::OpenNotifications: toggleNotificationCenter(); break;
      case CommandAction::CloseOverlay:
        if (_shell.overlay() != Overlay::None)
        {
          closeOverlay();
          postActivityNotification(
            rt::NotificationSeverity::Info,
            std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiOverlayClosed)});
        }

        break;
      case CommandAction::ShowHelp:
        openOverlay(Overlay::Help);
        postActivityNotification(
          rt::NotificationSeverity::Info,
          std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiHelpOpened)});
        break;
      case CommandAction::RevealCurrentTrack: revealCurrentTrack(); break;
      case CommandAction::SetPresentation: _library.setPresentation(command.argument); break;
      case CommandAction::ClearFilter:
        _library.clearFilterDraft();
        applyFilter();
        break;
      case CommandAction::Reload: reloadActiveList(); break;
      case CommandAction::Scan: _libraryScan.start(); break;
      case CommandAction::ScanCancel: _libraryScan.cancel(); break;
      case CommandAction::SelectToggle: _library.toggleFocusedMark(); break;
      case CommandAction::SelectVisual: _library.toggleVisualSelection(); break;
      case CommandAction::SelectAll: _library.markAllTracks(); break;
      case CommandAction::SelectClear: _library.clearMarks(); break;
      case CommandAction::EditProperties: editSelectedTrackProperties(); break;
      case CommandAction::OpenSettings:
        cancelTransientInteractions();
        _library.commitVisualSelection();
        _shell.closeInput();
        _settings.open();
        break;
      case CommandAction::Play: playSelectedTrack(); break;
      case CommandAction::TogglePlayback: executePlaybackCommand(uimodel::PlaybackCommand::PlayPause); break;
      case CommandAction::Stop: executePlaybackCommand(uimodel::PlaybackCommand::Stop); break;
      case CommandAction::Previous: executePlaybackCommand(uimodel::PlaybackCommand::Previous); break;
      case CommandAction::Next: executePlaybackCommand(uimodel::PlaybackCommand::Next); break;
      case CommandAction::Shuffle: executePlaybackCommand(uimodel::PlaybackCommand::ToggleShuffle); break;
      case CommandAction::Repeat: executePlaybackCommand(uimodel::PlaybackCommand::CycleRepeat); break;
      case CommandAction::Back:
      case CommandAction::Forward:
        if (!_library.navigateHistory(command.action == CommandAction::Forward))
        {
          postActivityNotification(
            rt::NotificationSeverity::Info,
            std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiWorkspaceHistoryUnavailable)});
        }
        else
        {
          leaveNavigation();
        }

        break;
      case CommandAction::Quit: _requestExit(); break;
    }
  }

  void EventController::postActivityNotification(rt::NotificationSeverity const severity, std::string message)
  {
    auto const lifetime = severity == rt::NotificationSeverity::Info ? rt::NotificationLifetime::transient()
                                                                     : rt::NotificationLifetime::history();
    _notifications.post(
      rt::NotificationRequest{.severity = severity, .message = std::move(message), .lifetime = lifetime});
  }

  void EventController::refreshCommandCompletion()
  {
    if (!_shell.isInputActive())
    {
      _shell.clearCommandCompletion();
      return;
    }

    auto* callback =
      _shell.inputMode() == ShellInputMode::QuickFilter ? &_filterCompletionCallback : &_commandCompletionCallback;

    if (!*callback)
    {
      _shell.clearCommandCompletion();
      return;
    }

    _shell.setCommandCompletion((*callback)(_shell.inputDraft(), _shell.inputField().cursor()));
  }

  void EventController::scheduleFilterDebounce()
  {
    if (_shell.inputMode() != ShellInputMode::QuickFilter || !_shell.isInputTouched())
    {
      return;
    }

    cancelFilterDebounce();
    auto const generation = _filterDebounceGeneration;
    _filterDebounceTask = _asyncRuntime.spawnCancellable(
      [runtime = &_asyncRuntime, owner = this, generation](std::stop_token const stopToken)
      { return waitForFilterDebounceAsync(runtime, owner, generation, stopToken); },
      "TUI Quick-filter debounce");
  }

  void EventController::cancelFilterDebounce() noexcept
  {
    _filterDebounceTask.reset();
    ++_filterDebounceGeneration;
  }

  void EventController::applyPendingFilter(std::uint64_t const generation)
  {
    if (generation != _filterDebounceGeneration || _shell.inputMode() != ShellInputMode::QuickFilter ||
        !_shell.isInputTouched())
    {
      return;
    }

    _library.setFilterDraft(_shell.inputDraft());
    applyFilter(false);
  }

  void EventController::closeQuickFilter(bool const acceptCompletion)
  {
    cancelFilterDebounce();

    if (!acceptCompletion && !_shell.isInputTouched())
    {
      _shell.closeInput();
      return;
    }

    if (acceptCompletion)
    {
      _shell.tryApplyCommandCompletion();
    }

    _library.setFilterDraft(_shell.inputDraft());
    applyFilter();
    _shell.rememberInput();
    _shell.closeInput();
  }

  async::Task<void> EventController::waitForFilterDebounceAsync(async::Runtime* const runtime,
                                                                EventController* const owner,
                                                                std::uint64_t const generation,
                                                                std::stop_token const stopToken)
  {
    co_await runtime->sleepForAsync(kFilterDebounceInterval, stopToken);
    co_await runtime->resumeOnCallbackExecutorAsync(stopToken);
    owner->applyPendingFilter(generation);
  }

  bool EventController::tryHandleMouse(ftxui::Mouse const& mouse)
  {
    auto const modalInputActive = _shell.isInputActive() || isModalOverlay(_shell.overlay());

    // A gesture aimed at the workspace cannot be finished across a surface that
    // took the workspace away, whichever of the two arrived first.
    if (modalInputActive && hasWorkspaceGesture())
    {
      cancelWorkspaceGestures();
      return false;
    }

    if (_shell.isInputActive())
    {
      return tryHandleInputMouse(mouse);
    }

    if (isLeftPress(mouse) && containsMouse(_hitRegions.overlayPanel.closeBox, mouse))
    {
      closeOverlay();
      return true;
    }

    // An admitted gesture owns pointer motion and release across workspace panes.
    if (auto const optHandled = handleActiveMouseDrag(mouse); optHandled)
    {
      return *optHandled;
    }

    if (auto const optHandled = tryHandleNavigationMouse(mouse); optHandled)
    {
      return *optHandled;
    }

    if (auto const optHandled = handleMouseWheel(mouse); optHandled)
    {
      return *optHandled;
    }

    if (mouse.motion == ftxui::Mouse::Moved)
    {
      return tryHandleMouseMove(mouse);
    }

    if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Pressed)
    {
      return false;
    }

    if (isOverlayActive(_shell.overlay()) && containsMouse(_hitRegions.overlayPanel.box, mouse))
    {
      _lastClickedTrack = kInvalidTrackId;
      tryHandleOverlayPress(mouse);
      return true;
    }

    if (isPopoverOverlay(_shell.overlay()) && !_hitRegions.overlayPanel.box.IsEmpty())
    {
      _lastClickedTrack = kInvalidTrackId;
      closeOverlay();
      return true;
    }

    if (auto const optHandled = handleSeekRailPress(mouse, modalInputActive); optHandled)
    {
      _lastClickedTrack = kInvalidTrackId;
      return *optHandled;
    }

    if (isLeftPress(mouse) && containsMouse(_hitRegions.trackTableBox, mouse))
    {
      leaveNavigation();
    }

    if (auto const optHandled = handleColumnResizePress(mouse); optHandled)
    {
      _lastClickedTrack = kInvalidTrackId;
      return *optHandled;
    }

    if (auto const optHandled = handleScrollbarPress(mouse); optHandled)
    {
      _lastClickedTrack = kInvalidTrackId;
      return *optHandled;
    }

    if (auto const optHandled = handleSectionPress(mouse); optHandled)
    {
      _lastClickedTrack = kInvalidTrackId;
      return *optHandled;
    }

    if (auto const optHandled = handleButtonPress(mouse); optHandled)
    {
      _lastClickedTrack = kInvalidTrackId;
      return *optHandled;
    }

    if (tryHandleOverlayPress(mouse))
    {
      return true;
    }

    return !isModalOverlay(_shell.overlay()) && tryHandleTrackPress(mouse);
  }

  bool EventController::tryHandleInputMouse(ftxui::Mouse const& mouse)
  {
    if (mouse.motion == ftxui::Mouse::Moved)
    {
      return tryHandleMouseMove(mouse);
    }

    if (isLeftPress(mouse) && containsMouse(_hitRegions.inputPanel.closeBox, mouse))
    {
      return tryHandleCommandEvent(ftxui::Event::Escape);
    }

    if (auto const& inputHit = _hitRegions.completion; isLeftPress(mouse) && containsMouse(inputHit.inputBox, mouse))
    {
      if (inputHit.draft == _shell.inputDraft() && inputHit.cursor == _shell.inputField().cursor() &&
          _shell.tryMoveInputCursor(mouse.x - inputHit.inputOrigin.x_min))
      {
        refreshCommandCompletion();
      }

      return true;
    }

    if (!containsMouse(_hitRegions.inputPanel.box, mouse))
    {
      if (isLeftPress(mouse) && !_hitRegions.inputPanel.box.IsEmpty())
      {
        _qualityHoverVisible = false;
        return tryHandleCommandEvent(ftxui::Event::Escape);
      }

      return false;
    }

    if (auto const wheel = mouseWheelDirection(mouse); wheel != 0)
    {
      _shell.tryMoveCommandCompletionByPage(wheel);
      return true;
    }

    if (!isLeftPress(mouse))
    {
      return false;
    }

    auto const& hit = _hitRegions.completion;
    auto const& optCompletion = _shell.commandCompletion();
    auto const optRow = mouseRowAt(hit.rows, mouse);

    if (!optRow || !optCompletion || hit.draft != _shell.inputDraft() || hit.cursor != _shell.inputField().cursor() ||
        hit.replaceBegin != optCompletion->replaceBegin || hit.replaceEnd != optCompletion->replaceEnd ||
        *optRow >= optCompletion->items.size() || *optRow >= hit.insertions.size() ||
        hit.insertions[*optRow] != optCompletion->items[*optRow].insertText)
    {
      return true;
    }

    _shell.tryMoveCommandCompletion(static_cast<std::int32_t>(*optRow) - _shell.commandCompletionSelection());
    tryHandleCommandEvent(ftxui::Event::Tab);

    if (_shell.inputMode() == ShellInputMode::Command && parseCommand(_shell.inputDraft()))
    {
      tryHandleCommandEvent(ftxui::Event::Return);
    }

    return true;
  }

  bool EventController::tryHandleTrackPress(ftxui::Mouse const& mouse)
  {
    for (auto const& hit : _hitRegions.trackRows)
    {
      if (!containsMouse(hit.box, mouse))
      {
        continue;
      }

      if (auto const& tracks = _library.tracks(); hit.rowIndex < 0 ||
                                                  static_cast<std::size_t>(hit.rowIndex) >= tracks.size() ||
                                                  tracks[hit.rowIndex].id != hit.id)
      {
        _lastClickedTrack = kInvalidTrackId;
        return true;
      }

      if (mouse.shift && !_library.isVisualSelectionActive())
      {
        _library.toggleVisualSelection();
      }
      else if (!mouse.shift)
      {
        _library.commitVisualSelection();

        if (!mouse.control)
        {
          _library.clearMarks();
        }
      }

      _library.setSelectedTrackIndex(hit.rowIndex);

      if (mouse.control)
      {
        _library.toggleFocusedMark();
      }

      auto const now = std::chrono::steady_clock::now();
      auto const isDoubleClick = !mouse.control && !mouse.shift && _lastClickedTrack == hit.id &&
                                 _lastClickedList == _library.currentListId() &&
                                 now - _lastTrackClickTime <= std::chrono::milliseconds{400};
      _lastClickedTrack = mouse.control || mouse.shift || isDoubleClick ? kInvalidTrackId : hit.id;
      _lastClickedList = _library.currentListId();
      _lastTrackClickTime = now;

      if (isDoubleClick)
      {
        playSelectedTrack();
      }

      return true;
    }

    _lastClickedTrack = kInvalidTrackId;
    return false;
  }

  std::optional<bool> EventController::handleActiveMouseDrag(ftxui::Mouse const& mouse)
  {
    if (std::holds_alternative<SeekRailDrag>(_workspaceGesture))
    {
      if (mouse.motion == ftxui::Mouse::Moved || mouse.motion == ftxui::Mouse::Released)
      {
        syncSeekSlider();
        auto const elapsed = seekRailElapsed(mouse.x);
        auto const update = mouse.motion == ftxui::Mouse::Released ? _seekSlider.endPointerInteraction(elapsed)
                                                                   : _seekSlider.valueChanged(elapsed);
        applySeekUpdate(update);

        if (mouse.motion == ftxui::Mouse::Released)
        {
          _workspaceGesture = std::monostate{};
        }

        return true;
      }

      _workspaceGesture = std::monostate{};
      _seekSlider.reset();
    }

    if (std::holds_alternative<TrackScrollbarDrag>(_workspaceGesture))
    {
      if (mouse.motion == ftxui::Mouse::Moved || mouse.motion == ftxui::Mouse::Released)
      {
        auto const handled = trySelectTrackFromScrollbar(mouse.y);

        if (mouse.motion == ftxui::Mouse::Released)
        {
          _workspaceGesture = std::monostate{};
        }

        return handled;
      }

      _workspaceGesture = std::monostate{};
    }

    if (std::holds_alternative<TrackColumnResizeDrag>(_workspaceGesture))
    {
      return tryHandleTrackColumnResizeDrag(mouse);
    }

    return std::nullopt;
  }

  bool EventController::tryHandleTrackColumnResizeDrag(ftxui::Mouse const& mouse)
  {
    if (mouse.motion != ftxui::Mouse::Moved && mouse.motion != ftxui::Mouse::Released)
    {
      cancelColumnResize();
      return false;
    }

    AO_EXPECTS(std::holds_alternative<TrackColumnResizeDrag>(_workspaceGesture));
    auto const drag = std::get<TrackColumnResizeDrag>(_workspaceGesture);

    if (_library.currentListId() != drag.listId || _library.trackRowsRevision() != drag.rowsRevision)
    {
      cancelColumnResize();
      return false;
    }

    auto const handleIt =
      std::ranges::find(_hitRegions.trackColumnResizeHandles, drag.field, &TrackColumnResizeHandle::field);

    if (handleIt == _hitRegions.trackColumnResizeHandles.end())
    {
      cancelColumnResize();
      return false;
    }

    auto const columns = drag.startColumns + mouse.x - drag.startX;
    _trackColumnResizePreview.listId = drag.listId;
    _trackColumnResizePreview.layout = resizeTerminalTrackColumnLayout(_library.activePresentation(),
                                                                       _trackColumnLayouts.layoutForList(drag.listId),
                                                                       drag.field,
                                                                       columns,
                                                                       handleIt->availableColumns);

    if (mouse.motion == ftxui::Mouse::Released)
    {
      _trackColumnLayouts.updateLayout(drag.listId, _trackColumnResizePreview.layout);
      cancelColumnResize();
    }

    return true;
  }

  std::optional<bool> EventController::handleMouseWheel(ftxui::Mouse const& mouse)
  {
    auto const wheel = mouseWheelDirection(mouse);

    if (wheel != 0)
    {
      _lastClickedTrack = kInvalidTrackId;
    }

    if (wheel != 0 && containsMouse(_hitRegions.overlayPanel.box, mouse))
    {
      switch (_shell.overlay())
      {
        case Overlay::PresentationPanel: tryMoveOverlaySelection(wheel); break;
        case Overlay::OutputDevices: _outputDevices.tryMoveSelection(wheel); break;
        case Overlay::DetailPanel:
        case Overlay::QualityPanel:
        case Overlay::Help:
        case Overlay::Notifications:
          _shell.scrollOverlay(wheel * _preferences.wheelStep,
                               _hitRegions.overlayPanel.contentBox.y_max - _hitRegions.overlayPanel.contentBox.y_min);
          break;
        case Overlay::None: return false;
      }

      return true;
    }

    if (wheel != 0 && !isModalOverlay(_shell.overlay()) && contains(_hitRegions.volumeBox, mouse.x, mouse.y))
    {
      _volumeViewModel.adjustVolume(-static_cast<float>(wheel * _preferences.volumePercent) / 100.0F);
      return true;
    }

    if ((mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) &&
        mouse.motion == ftxui::Mouse::Pressed)
    {
      if (!isModalOverlay(_shell.overlay()) && contains(_hitRegions.trackTableBox, mouse.x, mouse.y))
      {
        auto const delta = mouse.button == ftxui::Mouse::WheelUp ? -_preferences.wheelStep : _preferences.wheelStep;
        _library.moveTrackSelection(delta);
        return true;
      }

      return false;
    }

    return std::nullopt;
  }

  bool EventController::tryHandleMouseMove(ftxui::Mouse const& mouse)
  {
    auto const buttonHit =
      _hitRegions.hitTestButton(mouse.x,
                                mouse.y,
                                HitTestContext{.isTextInputActive = _shell.isInputActive(),
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

  std::optional<bool> EventController::handleSeekRailPress(ftxui::Mouse const& mouse, bool const modalInputActive)
  {
    if (modalInputActive || !contains(_hitRegions.seekRailBox, mouse.x, mouse.y))
    {
      return std::nullopt;
    }

    syncSeekSlider();

    if (!_seekSlider.tryBeginPointerInteraction())
    {
      return false;
    }

    _workspaceGesture = SeekRailDrag{};
    applySeekUpdate(_seekSlider.valueChanged(seekRailElapsed(mouse.x)));
    return true;
  }

  std::optional<bool> EventController::handleColumnResizePress(ftxui::Mouse const& mouse)
  {
    if (isModalOverlay(_shell.overlay()))
    {
      return std::nullopt;
    }

    auto const handleIt = std::ranges::find_if(_hitRegions.trackColumnResizeHandles,
                                               [&](TrackColumnResizeHandle const& handle)
                                               { return containsTrackColumnResizeEdge(handle, mouse.x, mouse.y); });

    if (handleIt == _hitRegions.trackColumnResizeHandles.end())
    {
      return std::nullopt;
    }

    if (_hitRegions.trackTableRevision != _library.trackRowsRevision())
    {
      return true;
    }

    _trackColumnResizePreview = {};
    _workspaceGesture = TrackColumnResizeDrag{
      .field = handleIt->field,
      .startX = mouse.x,
      .startColumns = handleIt->columns,
      .listId = _library.currentListId(),
      .rowsRevision = _hitRegions.trackTableRevision,
    };
    return true;
  }

  std::optional<bool> EventController::handleScrollbarPress(ftxui::Mouse const& mouse)
  {
    if (isModalOverlay(_shell.overlay()) || !containsTrackScrollbar(_hitRegions.trackTableBox, mouse.x, mouse.y))
    {
      return std::nullopt;
    }

    _workspaceGesture = TrackScrollbarDrag{};

    if (!trySelectTrackFromScrollbar(mouse.y))
    {
      _workspaceGesture = std::monostate{};
      return false;
    }

    return true;
  }

  std::optional<bool> EventController::handleSectionPress(ftxui::Mouse const& mouse)
  {
    if (isModalOverlay(_shell.overlay()))
    {
      return std::nullopt;
    }

    auto const hitRegionIt = std::ranges::find_if(_hitRegions.trackSectionRows,
                                                  [&](TrackSectionRowHitRegion const& hitRegion)
                                                  { return contains(hitRegion.box, mouse.x, mouse.y); });

    if (hitRegionIt == _hitRegions.trackSectionRows.end())
    {
      return std::nullopt;
    }

    if (_hitRegions.trackTableRevision != _library.trackRowsRevision() || hitRegionIt->sectionIndex < 0 ||
        static_cast<std::size_t>(hitRegionIt->sectionIndex) >= _library.sections().size())
    {
      postActivityNotification(
        rt::NotificationSeverity::Warning,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiSectionUnavailable)});
      return true;
    }

    _library.selectSection(hitRegionIt->sectionIndex);
    return true;
  }

  std::optional<bool> EventController::handleButtonPress(ftxui::Mouse const& mouse)
  {
    if (!isModalOverlay(_shell.overlay()))
    {
      if (_library.isVisualSelectionActive() && containsMouse(_hitRegions.cancelSelectionBox, mouse))
      {
        _library.cancelVisualSelection();
        return true;
      }

      if (containsMouse(_hitRegions.shuffleBox, mouse))
      {
        executeKeyAction(KeyAction::PlaybackShuffle);
        return true;
      }

      if (containsMouse(_hitRegions.repeatBox, mouse))
      {
        executeKeyAction(KeyAction::PlaybackRepeat);
        return true;
      }

      for (auto const& hit : _hitRegions.statusActions)
      {
        if (containsMouse(hit.box, mouse))
        {
          executeKeyAction(hit.action);
          return true;
        }
      }
    }

    if (!isModalOverlay(_shell.overlay()) && contains(_hitRegions.volumeBox, mouse.x, mouse.y))
    {
      _volumeViewModel.toggleMuted();
      return true;
    }

    if (contains(_hitRegions.settingsButtonBox, mouse.x, mouse.y))
    {
      runCommand(Command{.action = CommandAction::OpenSettings});
      return true;
    }

    if (contains(_hitRegions.outputDeviceButtonBox, mouse.x, mouse.y))
    {
      toggleOutputDevices();
      return true;
    }

    if (contains(_hitRegions.soulButtonBox, mouse.x, mouse.y))
    {
      executePlaybackCommand(uimodel::PlaybackCommand::PlayPause);
      return true;
    }

    if (contains(_hitRegions.libraryButtonBox, mouse.x, mouse.y))
    {
      toggleLists();
      return true;
    }

    if (contains(_hitRegions.presentationButtonBox, mouse.x, mouse.y))
    {
      togglePresentationPanel();
      return true;
    }

    if (contains(_hitRegions.activityStatusBox, mouse.x, mouse.y))
    {
      auto const& view = _activityStatusViewModel.viewState();

      if (uimodel::hasDetailContent(view.detail) || view.compact.hasDetails)
      {
        toggleNotificationCenter();
        return true;
      }

      if (view.compact.dismissible)
      {
        _activityStatusViewModel.dismissCompact();
        return true;
      }

      return true;
    }

    return std::nullopt;
  }

  bool EventController::tryHandlePresentationPress(ftxui::Mouse const& mouse)
  {
    auto const hit =
      std::ranges::find_if(_hitRegions.presentationRows,
                           [&](PresentationRowHitRegion const& row) { return contains(row.box, mouse.x, mouse.y); });

    if (hit == _hitRegions.presentationRows.end())
    {
      return false;
    }

    auto const& entries = _library.presentationEntries();

    if (hit->rowIndex < 0 || static_cast<std::size_t>(hit->rowIndex) >= entries.size())
    {
      return true;
    }

    if (auto const& entry = entries[hit->rowIndex];
        (!hit->presentationId.empty() && entry.id != hit->presentationId) || !_shell.listSearch().matches(entry.label))
    {
      return true;
    }

    if (_library.trySetSelectedPresentation(hit->rowIndex))
    {
      selectPresentation();
    }

    return true;
  }

  bool EventController::tryHandleOverlayPress(ftxui::Mouse const& mouse)
  {
    if (_shell.overlay() == Overlay::PresentationPanel)
    {
      return tryHandlePresentationPress(mouse);
    }

    if (_shell.overlay() == Overlay::Notifications)
    {
      auto const hitRegionIt = std::ranges::find_if(_hitRegions.notificationDetailRows,
                                                    [&](NotificationDetailRowHitRegion const& hitRegion)
                                                    { return contains(hitRegion.box, mouse.x, mouse.y); });

      if (hitRegionIt != _hitRegions.notificationDetailRows.end())
      {
        if (hitRegionIt->dismissible)
        {
          _activityStatusViewModel.hideDetailNotification(hitRegionIt->id);
          return true;
        }

        return true;
      }

      return false;
    }

    if (_shell.overlay() != Overlay::OutputDevices)
    {
      return false;
    }

    auto const hitRegionIt = std::ranges::find_if(
      _hitRegions.outputDeviceRows,
      [&](OutputDeviceRowHitRegion const& hitRegion)
      { return contains(hitRegion.box, mouse.x, mouse.y) || contains(hitRegion.secondaryBox, mouse.x, mouse.y); });

    if (hitRegionIt != _hitRegions.outputDeviceRows.end())
    {
      if (hitRegionIt->rowIndex < 0 ||
          static_cast<std::size_t>(hitRegionIt->rowIndex) >= _outputDevices.viewState().rows.size())
      {
        return true;
      }

      auto const& row = _outputDevices.viewState().rows[static_cast<std::size_t>(hitRegionIt->rowIndex)];

      if (!matchesOutputDeviceRow(row, *hitRegionIt))
      {
        return true;
      }

      if (_outputDevices.trySelectRow(hitRegionIt->rowIndex))
      {
        closeOverlay();
      }

      return true;
    }

    return false;
  }

  void EventController::submitCommandInput()
  {
    if (_shell.inputMode() == ShellInputMode::QuickFilter)
    {
      closeQuickFilter(true);
      return;
    }

    auto optCommand = parseCommand(_shell.inputDraft());

    if ((!optCommand || _shell.isCompletionNavigated()) && _shell.tryApplyCommandCompletion())
    {
      optCommand = parseCommand(_shell.inputDraft());

      if (!optCommand)
      {
        refreshCommandCompletion();
        return;
      }
    }

    if (!optCommand)
    {
      if (_shell.inputDraft().empty())
      {
        _shell.closeInput();
      }
      else
      {
        postActivityNotification(
          rt::NotificationSeverity::Warning,
          i18n::requiredFormat(
            _library.textCatalog(), i18n::MessageId::TuiUnknownCommand, {{"command", _shell.inputDraft()}}));
      }

      return;
    }

    _shell.rememberInput();
    _shell.closeInput();
    runCommand(*optCommand);
  }

  bool EventController::tryHandleCommandEvent(ftxui::Event const& event)
  {
    if (event == ftxui::Event::Escape)
    {
      if (_shell.inputMode() == ShellInputMode::QuickFilter)
      {
        closeQuickFilter(false);
      }
      else
      {
        cancelFilterDebounce();
        _shell.closeInput();
      }

      return true;
    }

    if (event == ftxui::Event::Return)
    {
      submitCommandInput();
      return true;
    }

    if (event == ftxui::Event::Tab)
    {
      if (_shell.tryApplyCommandCompletion())
      {
        refreshCommandCompletion();
        scheduleFilterDebounce();
      }

      return true;
    }

    if (event == ftxui::Event::ArrowUp)
    {
      _shell.tryMoveCommandCompletion(-1);
      return true;
    }

    if (event == ftxui::Event::ArrowDown)
    {
      _shell.tryMoveCommandCompletion(1);
      return true;
    }

    if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
    {
      _shell.tryMoveCommandCompletionByPage(
        listNavigationDelta(event, navigationPageRows(_hitRegions.completion.listBox)).value_or(0));
      return true;
    }

    if (event == ftxui::Event::CtrlP || event == ftxui::Event::CtrlN)
    {
      if (_shell.tryMoveInputHistory(event == ftxui::Event::CtrlP ? -1 : 1))
      {
        refreshCommandCompletion();
        scheduleFilterDebounce();
      }

      return true;
    }

    auto const previousCursor = _shell.inputField().cursor();
    auto const edited = _shell.tryEditInput(event);

    if (edited || previousCursor != _shell.inputField().cursor())
    {
      refreshCommandCompletion();
    }

    if (edited)
    {
      scheduleFilterDebounce();
    }

    return true;
  }

  bool EventController::tryHandleListSearchEvent(ftxui::Event const& event)
  {
    if (_shell.isInputActive() || _shell.overlay() != Overlay::PresentationPanel)
    {
      return false;
    }

    if (!_shell.listSearch().tryHandleEvent(event))
    {
      return false;
    }

    tryMoveOverlaySelection(0);
    return true;
  }

  bool EventController::tryMoveOverlaySelection(std::int32_t const delta)
  {
    auto labels = std::vector<std::string>{};

    for (auto const& entry : _library.presentationEntries())
    {
      labels.push_back(entry.label);
    }

    auto const selected = _library.selectedPresentation();
    auto const optTarget = _shell.listSearch().selection(labels, selected, delta);

    if (!optTarget)
    {
      return false;
    }

    _library.movePresentationSelection(*optTarget - selected);

    return true;
  }

  bool EventController::tryHandleOverlayNavigation(ftxui::Event const& event)
  {
    auto const overlay = _shell.overlay();

    if (!isModalOverlay(overlay))
    {
      return false;
    }

    auto const picker = overlay == Overlay::PresentationPanel || overlay == Overlay::OutputDevices;
    auto pageRows = navigationPageRows(picker || overlay == Overlay::Help ? _hitRegions.overlayPanel.navigationBox
                                                                          : _hitRegions.overlayPanel.box);

    if (overlay == Overlay::OutputDevices)
    {
      auto const visibleRows = std::ranges::count_if(
        _hitRegions.outputDeviceRows, [](OutputDeviceRowHitRegion const& row) { return !row.box.IsEmpty(); });

      if (visibleRows > 0)
      {
        pageRows = static_cast<std::int32_t>(visibleRows);
      }
    }

    auto const optDelta = listNavigationDelta(event, pageRows, true);

    if (!optDelta)
    {
      return false;
    }

    switch (overlay)
    {
      case Overlay::PresentationPanel: tryMoveOverlaySelection(*optDelta); break;
      case Overlay::OutputDevices: _outputDevices.tryMoveSelection(*optDelta); break;
      case Overlay::Help:
      case Overlay::QualityPanel:
      case Overlay::Notifications:
        _shell.scrollOverlay(
          *optDelta, _hitRegions.overlayPanel.contentBox.y_max - _hitRegions.overlayPanel.contentBox.y_min);
        break;
      case Overlay::None:
      case Overlay::DetailPanel: return false;
    }

    return true;
  }

  bool EventController::tryHandleOverlayActivation(ftxui::Event const& event)
  {
    if (event == ftxui::Event::Return)
    {
      switch (_shell.overlay())
      {
        case Overlay::PresentationPanel:
          if (tryMoveOverlaySelection(0))
          {
            selectPresentation();
          }

          return true;
        case Overlay::OutputDevices: selectOutputDevice(); return true;
        default: break;
      }
    }

    if (_shell.overlay() == Overlay::Notifications && event == ftxui::Event::Character("x"))
    {
      if (_activityStatusViewModel.viewState().compact.dismissible)
      {
        _activityStatusViewModel.dismissCompact();
      }

      return true;
    }

    return false;
  }

  bool EventController::tryHandleOverlayEvent(ftxui::Event const& event)
  {
    // Detail follows the table focus and leaves the workspace drivable.
    if (!isModalOverlay(_shell.overlay()))
    {
      return false;
    }

    if (tryHandleOverlayNavigation(event) || tryHandleOverlayActivation(event))
    {
      return true;
    }

    auto const optAction = _keymapPlan.actionFor(event);

    switch (_shell.overlay())
    {
      case Overlay::QualityPanel:
        if (optAction == KeyAction::ToggleAudioPipeline)
        {
          toggleQualityPanel();
        }

        break;
      case Overlay::OutputDevices:
        if (optAction == KeyAction::ToggleOutputDevices)
        {
          toggleOutputDevices();
        }

        break;
      case Overlay::PresentationPanel:
        if (optAction == KeyAction::TogglePresentations)
        {
          togglePresentationPanel();
        }

        break;
      case Overlay::Notifications:
        if (optAction == KeyAction::ToggleNotifications)
        {
          toggleNotificationCenter();
        }

        break;
      case Overlay::Help:
        if (optAction == KeyAction::ShowHelp)
        {
          closeOverlay();
        }

        break;
      case Overlay::None:
      case Overlay::DetailPanel: break;
    }

    if (optAction && isPlaybackControl(*optAction))
    {
      executeKeyAction(*optAction);
    }

    return true;
  }

  bool EventController::tryHandleRootEvent(ftxui::Event const& event)
  {
    auto viewport = _hitRegions.trackTableBox;

    if (!viewport.IsEmpty())
    {
      ++viewport.y_min;
    }

    if (auto optDelta = listNavigationDelta(event, navigationPageRows(viewport)); optDelta)
    {
      if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
      {
        auto const selected = _library.selectedTrack();
        auto const visual = trackVisualRow(selected, _library.sections());
        auto const target = trackIndexForVisualRow(visual + *optDelta, _library.tracks().size(), _library.sections());
        _library.moveTrackSelection(target - selected);
      }
      else
      {
        _library.moveTrackSelection(*optDelta);
      }

      return true;
    }

    if (auto const optAction = _keymapPlan.actionFor(event); optAction)
    {
      executeKeyAction(*optAction);
      return true;
    }

    return false;
  }

  bool EventController::trySelectTrackFromScrollbar(std::int32_t const row)
  {
    if (_library.tracks().empty())
    {
      return false;
    }

    auto const target =
      scrollbarTrackIndex(_hitRegions.trackTableBox, row, _library.tracks().size(), _library.sections());
    _library.setSelectedTrackIndex(target);
    return true;
  }

  void EventController::syncSeekSlider()
  {
    auto const duration = _playback.snapshot().transport.duration;
    _seekSlider.applyViewState(duration, duration > std::chrono::milliseconds{0});
  }

  std::chrono::milliseconds EventController::seekRailElapsed(std::int32_t const column) const
  {
    auto const duration = _playback.snapshot().transport.duration;

    if (duration <= std::chrono::milliseconds{0})
    {
      return std::chrono::milliseconds{0};
    }

    auto const& seekRailBox = _hitRegions.seekRailBox;
    auto const railColumns = std::max(1, seekRailBox.x_max - seekRailBox.x_min + 1);
    auto const denominator = std::max(1, railColumns - 1);
    auto const relativeColumn = std::clamp(column - seekRailBox.x_min, 0, denominator);
    auto const fraction = static_cast<double>(relativeColumn) / static_cast<double>(denominator);
    auto const elapsed =
      static_cast<std::chrono::milliseconds::rep>(std::llround(static_cast<double>(duration.count()) * fraction));
    return std::chrono::milliseconds{elapsed};
  }

  void EventController::applySeekUpdate(uimodel::SeekSliderUpdate const& update)
  {
    switch (update.action)
    {
      case uimodel::SeekSliderAction::None: return;
      case uimodel::SeekSliderAction::Preview: _seekViewModel.seekPreview(update.elapsed); return;
      case uimodel::SeekSliderAction::Commit: _seekViewModel.seekFinal(update.elapsed); return;
    }
  }

  void EventController::cancelColumnResize()
  {
    _workspaceGesture = std::monostate{};
    _trackColumnResizePreview = {};
  }

  bool EventController::hasWorkspaceGesture() const noexcept
  {
    return !std::holds_alternative<std::monostate>(_workspaceGesture);
  }

  void EventController::cancelWorkspaceGestures()
  {
    if (std::holds_alternative<SeekRailDrag>(_workspaceGesture) && _seekSlider.hasPendingFinalSeek())
    {
      _seekViewModel.seekFinal(_playback.snapshot().transport.elapsed);
    }

    _seekSlider.reset();
    _workspaceGesture = std::monostate{};
    _trackColumnResizePreview = {};
  }

  void EventController::openOverlay(Overlay const overlay)
  {
    cancelWorkspaceGestures();
    _lastClickedTrack = kInvalidTrackId;
    _shell.openOverlay(overlay);
  }

  void EventController::closeOverlay()
  {
    cancelWorkspaceGestures();
    _qualityHoverVisible = false;
    _shell.closeOverlay();
  }
} // namespace ao::tui
