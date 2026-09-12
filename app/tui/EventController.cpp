// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EventController.h"

#include "Command.h"
#include "GoToMenu.h"
#include "HitRegions.h"
#include "Keymap.h"
#include "LibraryChooser.h"
#include "LibraryController.h"
#include "LibraryNavigation.h"
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
#include "TrackPropertiesEditor.h"
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
        case Overlay::Help:
        case Overlay::ListChooser:
        case Overlay::GoTo: return true;
        case Overlay::None: return false;
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

    /// Whether a temporary popover occupies the screen.
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

  GoToMenuState EventController::goToMenuState() const
  {
    return {.nowPlaying = _playback.snapshot().transport.nowPlaying,
            .canGoBack = _library.canNavigateBack(),
            .canGoForward = _library.canNavigateForward()};
  }

  bool EventController::tryHandleEvent(ftxui::Event const& event)
  {
    syncWorkspaceGeometry();

    if (hasWorkspaceGesture() &&
        (_settings.isActive() || _trackEdit.isActive() || _shell.isInputActive() || isModalOverlay(_shell.overlay())))
    {
      cancelWorkspaceGestures();
    }

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
    if (isExitWaiting())
    {
      return true;
    }

    if (event.is_mouse() && !_preferences.mouseEnabled)
    {
      return true;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;
      _optLastMouse = mouseEvent.mouse();
    }

    // An open editor owns the whole surface, including keys and mouse events
    // it has no use for, so nothing behind it can act on stale geometry.
    if (_settings.tryHandleEvent(event) || _trackEdit.tryHandleEvent(event))
    {
      return true;
    }

    if (tryHandleKeyboardPanelResize(event))
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

    if (std::holds_alternative<PanelResizeInteraction>(_workspaceGesture))
    {
      // Pointer resize owns motion/release; ordinary keys act after rollback.
      // Escape only cancels, so it cannot also clear the workspace selection.
      cancelWorkspaceGestures();

      if (event == ftxui::Event::Escape)
      {
        return true;
      }
    }

    if (tryHandlePanelResizeKey(event))
    {
      return true;
    }

    if (tryHandleListSearchEvent(event))
    {
      return true;
    }

    if (tryHandleDetailEvent(event) || tryHandleNavigationEvent(event))
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

  bool EventController::tryRetireHover()
  {
    // Layout changes may retire hover, but must not resurrect a keyboard-cancelled
    // interaction or take pointer ownership from an active drag.
    if (!_optLastMouse || hasWorkspaceGesture() || (_hoveredButton == HoveredButton::None && !_qualityHoverVisible))
    {
      return false;
    }

    auto const hit = _hitRegions.hitTestButton(
      _optLastMouse->x,
      _optLastMouse->y,
      {.isTextInputActive = _shell.isInputActive() || _settings.isActive() || _trackEdit.isActive() ||
                            !_preferences.mouseEnabled || isExitWaiting(),
       .isOverlayActive = isOverlayActive(_shell.overlay())});

    if (hit.hoveredButton != _hoveredButton || hit.isQualityHoverVisible != _qualityHoverVisible)
    {
      _hoveredButton = HoveredButton::None;
      _qualityHoverVisible = false;
      return true;
    }

    return false;
  }

  void EventController::cancelTransientInteractions()
  {
    if (_shell.overlay() == Overlay::GoTo)
    {
      _shell.closeOverlay();
    }

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
    cancelWorkspaceGestures();
    _lastClickedTrack = kInvalidTrackId;
    _qualityHoverVisible = false;
    _shell.toggleDetail();
    _hoveredButton = HoveredButton::None;
    _hitRegions.detailToggleBox = kEmptyMouseBox;
    _hitRegions.detailDividerBox = kEmptyMouseBox;
    _hitRegions.detailPanel = {};
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

  void EventController::editSelectedTrackProperties(TrackEditorMode const mode)
  {
    if (!_trackEdit.tryOpen(_library.selectedTrackIds(), mode))
    {
      return;
    }

    // Both editor modes consume every event, so cancel pending workspace
    // gestures and overlays before handing input to the editor.
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

  void EventController::activateGoTo(CommandAction const action)
  {
    if (!canActivateGoTo(goToMenuState(), action))
    {
      return;
    }

    closeOverlay();
    runCommand({.action = action});
  }

  bool EventController::tryHandleGoToEvent(ftxui::Event const& event)
  {
    auto const commands = goToCommands();

    for (auto const& command : commands)
    {
      if (event == ftxui::Event::Character(std::string{command.goToKey}))
      {
        activateGoTo(command.action);
        return true;
      }
    }

    if (auto const optDelta = listNavigationDelta(event, static_cast<std::int32_t>(commands.size()), true); optDelta)
    {
      _shell.scrollOverlay(*optDelta, static_cast<std::int32_t>(commands.size()) - 1);
    }
    else if (event == ftxui::Event::Return)
    {
      activateGoTo(commands[_shell.overlayScroll()].action);
    }
    else
    {
      // An unmatched suffix cancels this menu without becoming a workspace key.
      closeOverlay();
    }

    return true;
  }

  void EventController::navigateCurrentMetadata(bool const album)
  {
    auto const& track = _playback.snapshot().transport.nowPlaying;

    if (track.trackId == kInvalidTrackId || (!album && track.artist.empty()))
    {
      return;
    }

    auto const res = album ? _library.revealAlbum(track.trackId) : _library.navigateToArtist(track.artist);

    if (!res)
    {
      APP_LOG_ERROR("Failed to navigate TUI playback metadata: {}", res.error().message);
      postActivityNotification(
        rt::NotificationSeverity::Warning,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiLibraryCurrentTrackNotInView)});
      return;
    }

    leaveNavigation();
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

  void EventController::reportPlaybackControlUnavailable()
  {
    postActivityNotification(
      rt::NotificationSeverity::Warning,
      std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiPlaybackControlUnavailable)});
  }

  void EventController::executePlaybackCommand(uimodel::PlaybackCommand const command)
  {
    if (!_playbackActions.tryExecute(command))
    {
      if (command != uimodel::PlaybackCommand::Stop)
      {
        reportPlaybackControlUnavailable();
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
      case BeginPanelResize: beginKeyboardPanelResize(); break;
      case SwitchWorkspaceFocus: switchWorkspaceFocus(); break;
      case FocusDetails:
        leaveNavigation();
        cancelTransientInteractions();
        _shell.focusDetail();
        break;
      case OpenCommandPalette:
      case OpenQuickFilter:
        if (action == OpenQuickFilter)
        {
          leaveNavigation();
        }

        cancelWorkspaceGestures();
        _hoveredButton = HoveredButton::None;
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
      case TogglePinnedLists:
      case ToggleLists:
      case ToggleDetails:
      case ToggleAudioPipeline:
      case ToggleOutputDevices:
      case TogglePresentations:
      case ToggleNotifications:
      case ShowHelp:
      case OpenGoTo:
      case OpenCurrentArtist:
      case OpenCurrentAlbum:
      case WorkspaceBack:
      case WorkspaceForward:
      case RevealCurrentTrack:
      case ClearFilter:
      case Reload:
      case Scan:
      case ScanCancel:
      case SelectToggle:
      case SelectVisual:
      case SelectAll:
      case SelectClear:
      case EditTags:
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
      case CommandAction::TogglePinnedLists: togglePinnedLists(); break;
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
      case CommandAction::OpenGoTo: openOverlay(Overlay::GoTo); break;
      case CommandAction::RevealCurrentTrack: revealCurrentTrack(); break;
      case CommandAction::OpenCurrentArtist: navigateCurrentMetadata(false); break;
      case CommandAction::OpenCurrentAlbum: navigateCurrentMetadata(true); break;
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
      case CommandAction::EditProperties: editSelectedTrackProperties(TrackEditorMode::Properties); break;
      case CommandAction::EditTags: editSelectedTrackProperties(TrackEditorMode::Tags); break;
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

    // An admitted gesture owns pointer motion and release across workspace panes.
    if (auto const optHandled = handleActiveMouseDrag(mouse); optHandled)
    {
      return *optHandled;
    }

    if (tryBeginPanelResize(mouse))
    {
      return true;
    }

    if (auto const optHandled = tryHandleNavigationMouse(mouse); optHandled)
    {
      return *optHandled;
    }

    if (auto const optHandled = tryHandleDetailMouse(mouse); optHandled)
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

    if (_shell.overlay() == Overlay::GoTo && tryHandleGoToPress(mouse, _hitRegions.goToStatus))
    {
      return true;
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
      _shell.focusTracks();
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
    if (std::holds_alternative<PanelResizeInteraction>(_workspaceGesture))
    {
      return tryHandlePanelResizeDrag(mouse);
    }

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
        case Overlay::GoTo: _shell.scrollOverlay(wheel, static_cast<std::int32_t>(goToCommands().size()) - 1); break;
        case Overlay::ListChooser:
        case Overlay::PresentationPanel: tryMoveOverlaySelection(wheel); break;
        case Overlay::OutputDevices: _outputDevices.tryMoveSelection(wheel); break;
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

  bool EventController::tryHandlePlaybackMetadataPress(ftxui::Mouse const& mouse)
  {
    auto const& metadata = _hitRegions.playbackMetadata;
    auto const titleHit = containsMouse(metadata.title, mouse);
    auto const artistHit = containsMouse(metadata.artist, mouse);

    if (auto const albumHit = containsMouse(metadata.album, mouse); titleHit || artistHit || albumHit)
    {
      // A playback update must repaint before its new subject owns these cells.
      if (metadata.nowPlaying == _playback.snapshot().transport.nowPlaying)
      {
        if (titleHit)
        {
          revealCurrentTrack();
        }
        else
        {
          navigateCurrentMetadata(albumHit);
        }
      }

      return true;
    }

    return false;
  }

  bool EventController::tryHandlePlaybackModePress(ftxui::Mouse const& mouse)
  {
    if (!containsMouse(_hitRegions.playbackModeBox, mouse))
    {
      return false;
    }

    if (!_playbackActions.isEnabled(uimodel::PlaybackCommand::ToggleShuffle) ||
        !_playbackActions.isEnabled(uimodel::PlaybackCommand::CycleRepeat))
    {
      reportPlaybackControlUnavailable();
      return true;
    }

    auto const& current = _playback.snapshot().succession;

    if (auto const optPreset = playbackModePreset(current.shuffle, current.repeat); optPreset)
    {
      _playback.commands().setPlaybackMode(optPreset->next.shuffle, optPreset->next.repeat);
    }

    return true;
  }

  std::optional<bool> EventController::handleButtonPress(ftxui::Mouse const& mouse)
  {
    if (!isModalOverlay(_shell.overlay()))
    {
      if (tryHandlePlaybackMetadataPress(mouse))
      {
        return true;
      }

      if (_library.isVisualSelectionActive() && containsMouse(_hitRegions.cancelSelectionBox, mouse))
      {
        _library.cancelVisualSelection();
        return true;
      }

      if (tryHandlePlaybackModePress(mouse))
      {
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

  bool EventController::tryHandleLibraryPress(ftxui::Mouse const& mouse)
  {
    auto const hit = std::ranges::find_if(
      _hitRegions.libraryRows, [&](LibraryRowHitRegion const& row) { return containsMouse(row.box, mouse); });

    if (hit == _hitRegions.libraryRows.end())
    {
      return false;
    }

    auto const& entries = _library.libraryEntries();
    auto const entry = std::ranges::find(entries, hit->id, &LibraryNavEntry::id);

    if (entry == entries.end())
    {
      return true;
    }

    auto const index = static_cast<std::int32_t>(entry - entries.begin());

    if (_shell.listSearch().matches(_library.libraryLabels()[index]))
    {
      _library.selectListRow(index);
      selectLibraryList();
    }

    return true;
  }

  void EventController::selectLibraryList()
  {
    auto const& entries = _library.libraryEntries();

    if (entries.empty())
    {
      return;
    }

    if (auto const res = _library.openList(entries[_library.selectedList()].id); !res)
    {
      postActivityNotification(
        rt::NotificationSeverity::Warning,
        i18n::requiredFormat(
          _library.textCatalog(), i18n::MessageId::TuiNavigationOpenFailed, {{"detail", res.error().message}}));
      return;
    }

    closeOverlay();
    _shell.focusTracks();
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

  bool EventController::tryHandleGoToPress(ftxui::Mouse const& mouse, GoToMenuHitRegions const& hitRegions)
  {
    if (containsMouse(hitRegions.cancelBox, mouse))
    {
      closeOverlay();
      return true;
    }

    auto const hit = std::ranges::find_if(
      hitRegions.rows, [&](GoToMenuRowHitRegion const& row) { return containsMouse(row.box, mouse); });

    if (hit == hitRegions.rows.end())
    {
      return false;
    }

    if (hitRegions.state == goToMenuState())
    {
      activateGoTo(hit->action);
    }

    return true;
  }

  bool EventController::tryHandleOverlayPress(ftxui::Mouse const& mouse)
  {
    if (_shell.overlay() == Overlay::GoTo)
    {
      return tryHandleGoToPress(mouse, _hitRegions.goToMenu);
    }

    if (_shell.overlay() == Overlay::ListChooser)
    {
      return tryHandleLibraryPress(mouse);
    }

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
    if (_shell.isInputActive() ||
        (_shell.overlay() != Overlay::PresentationPanel && _shell.overlay() != Overlay::ListChooser))
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
    if (_shell.overlay() == Overlay::ListChooser)
    {
      auto const optTarget = _shell.listSearch().selection(_library.libraryLabels(), _library.selectedList(), delta);

      if (!optTarget)
      {
        return false;
      }

      _library.selectListRow(*optTarget);
      return true;
    }

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

    auto const picker =
      overlay == Overlay::ListChooser || overlay == Overlay::PresentationPanel || overlay == Overlay::OutputDevices;
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
      case Overlay::ListChooser:
      case Overlay::PresentationPanel: tryMoveOverlaySelection(*optDelta); break;
      case Overlay::OutputDevices: _outputDevices.tryMoveSelection(*optDelta); break;
      case Overlay::Help:
      case Overlay::QualityPanel:
      case Overlay::Notifications:
        _shell.scrollOverlay(
          *optDelta, _hitRegions.overlayPanel.contentBox.y_max - _hitRegions.overlayPanel.contentBox.y_min);
        break;
      case Overlay::GoTo:
      case Overlay::None: return false;
    }

    return true;
  }

  bool EventController::tryHandleOverlayActivation(ftxui::Event const& event)
  {
    if (event == ftxui::Event::Return)
    {
      switch (_shell.overlay())
      {
        case Overlay::ListChooser:
          if (tryMoveOverlaySelection(0))
          {
            selectLibraryList();
          }

          return true;
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
    // Workspace sidebars leave the keys to the track table.
    if (!isModalOverlay(_shell.overlay()))
    {
      return false;
    }

    if (_shell.overlay() == Overlay::GoTo)
    {
      return tryHandleGoToEvent(event);
    }

    if (tryHandleOverlayNavigation(event) || tryHandleOverlayActivation(event))
    {
      return true;
    }

    auto const optAction = _keymapPlan.actionFor(event);

    switch (_shell.overlay())
    {
      case Overlay::ListChooser:
        if (optAction == KeyAction::ToggleLists)
        {
          toggleLists();
        }
        else if (optAction == KeyAction::TogglePinnedLists)
        {
          togglePinnedLists();
        }

        break;
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
      case Overlay::GoTo:
      case Overlay::None: break;
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
    return _navigationScrollbarDrag || !std::holds_alternative<std::monostate>(_workspaceGesture);
  }

  void EventController::cancelWorkspaceGestures()
  {
    _navigationScrollbarDrag = false;

    if (std::holds_alternative<SeekRailDrag>(_workspaceGesture) && _seekSlider.hasPendingFinalSeek())
    {
      _seekViewModel.seekFinal(_playback.snapshot().transport.elapsed);
    }

    if (std::holds_alternative<PanelResizeInteraction>(_workspaceGesture))
    {
      _hoveredButton = HoveredButton::None;
    }

    _seekSlider.reset();
    _workspaceGesture = std::monostate{};
    _trackColumnResizePreview = {};
  }

  void EventController::openOverlay(Overlay const overlay)
  {
    cancelWorkspaceGestures();
    _lastClickedTrack = kInvalidTrackId;
    _qualityHoverVisible = false;
    _hoveredButton = HoveredButton::None;
    _shell.openOverlay(overlay);
  }

  void EventController::closeOverlay()
  {
    cancelWorkspaceGestures();
    _qualityHoverVisible = false;
    _hoveredButton = HoveredButton::None;
    _shell.closeOverlay();
  }
} // namespace ao::tui
