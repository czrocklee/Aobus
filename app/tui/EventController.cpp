// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EventController.h"

#include "LibraryController.h"
#include "LibraryScanController.h"
#include "NotificationCenterPanel.h"
#include "OutputDeviceController.h"
#include "OutputDevicePanel.h"
#include "PlaybackPanel.h"
#include "PresentationPanel.h"
#include "SelectionNavigation.h"
#include "ShellInteractionModel.h"
#include "TerminalTrackColumnLayout.h"
#include "TrackListEntry.h"
#include "TrackSection.h"
#include "TrackTable.h"
#include "TuiHitRegions.h"
#include "TuiKeymap.h"
#include <ao/Contract.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/playback/PlaybackCommands.h>
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
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kMouseWheelSelectionDelta = 3;
    constexpr auto kKeyboardSeekDelta = std::chrono::seconds{5};
    constexpr auto kFilterDebounceInterval = std::chrono::milliseconds{200};
    constexpr float kKeyboardVolumeDelta = 0.05F;
    constexpr std::int32_t kPageSelectionDelta = 10;
    constexpr std::int32_t kBoundarySelectionDelta = 1'000'000;

    std::optional<std::int32_t> listNavigationDelta(ftxui::Event const& event)
    {
      if (event == ftxui::Event::ArrowUp)
      {
        return -1;
      }

      if (event == ftxui::Event::ArrowDown)
      {
        return 1;
      }

      if (event == ftxui::Event::PageUp)
      {
        return -kPageSelectionDelta;
      }

      if (event == ftxui::Event::PageDown)
      {
        return kPageSelectionDelta;
      }

      if (event == ftxui::Event::Home)
      {
        return -kBoundarySelectionDelta;
      }

      if (event == ftxui::Event::End)
      {
        return kBoundarySelectionDelta;
      }

      return std::nullopt;
    }

    template<typename MoveSelection>
    bool handleListNavigation(ftxui::Event const& event, MoveSelection moveSelection)
    {
      auto const optDelta = listNavigationDelta(event);

      if (!optDelta)
      {
        return false;
      }

      moveSelection(*optDelta);
      return true;
    }

    bool playSelected(rt::PlaybackCommands& commands,
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
                                   TuiKeymapPlan const& keymapPlan,
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
    , _requestExit{std::move(bindings.requestExit)}
    , _commandCompletionCallback{std::move(bindings.commandCompletionCallback)}
    , _filterCompletionCallback{std::move(bindings.filterCompletionCallback)}
  {
  }

  void EventController::postActivityNotification(rt::NotificationSeverity const severity, std::string message)
  {
    auto const lifetime = severity == rt::NotificationSeverity::Info ? rt::NotificationLifetime::transient()
                                                                     : rt::NotificationLifetime::history();
    _notifications.post(
      rt::NotificationRequest{.severity = severity, .message = std::move(message), .lifetime = lifetime});
  }

  void EventController::openSelectedList()
  {
    auto result = _library.openSelectedList();

    if (result.opened)
    {
      closeOverlay();

      if (!result.status.empty())
      {
        postActivityNotification(rt::NotificationSeverity::Info, std::move(result.status));
      }

      return;
    }

    if (!result.status.empty())
    {
      postActivityNotification(rt::NotificationSeverity::Warning, std::move(result.status));
    }
  }

  void EventController::reloadActiveList()
  {
    _library.reloadActiveList();
  }

  void EventController::applyFilter(bool const reportError)
  {
    if (auto result = _library.applyFilter(); !result)
    {
      APP_LOG_ERROR("Failed to apply TUI filter: {}", result.error().message);

      if (reportError)
      {
        postActivityNotification(
          rt::NotificationSeverity::Error,
          i18n::requiredFormat(
            _library.textCatalog(), i18n::MessageId::TuiFilterFailed, {{"detail", result.error().message}}));
      }

      return;
    }

    if (reportError && !_library.filterError().empty())
    {
      postActivityNotification(rt::NotificationSeverity::Warning, _library.filterError());
    }
  }

  void EventController::toggleListChooser()
  {
    if (_shell.overlay() == Overlay::ListChooser)
    {
      closeOverlay();
      postActivityNotification(
        rt::NotificationSeverity::Info,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiListsClosed)});
      return;
    }

    openOverlay(Overlay::ListChooser);
    postActivityNotification(rt::NotificationSeverity::Info,
                             std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiListsOpened)});
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

  void EventController::selectOutputDevice()
  {
    _outputDevices.selectSelected();
    closeOverlay();
  }

  void EventController::selectPresentation()
  {
    _library.selectSelectedPresentation();
    closeOverlay();
  }

  void EventController::revealCurrentTrack()
  {
    _library.revealTrack(_playback.snapshot().transport.nowPlaying.trackId);
  }

  void EventController::playSelectedTrack()
  {
    if (!playSelected(_playback.commands(), _library.tracks(), _library.selectedTrack(), _library.activeViewId()))
    {
      postActivityNotification(
        rt::NotificationSeverity::Warning,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiPlaybackStartFailed)});
    }
  }

  void EventController::executePlaybackCommand(uimodel::PlaybackCommand const command)
  {
    if (!_playbackActions.execute(command) && command != uimodel::PlaybackCommand::Stop)
    {
      postActivityNotification(
        rt::NotificationSeverity::Warning,
        std::string{i18n::requiredText(_library.textCatalog(), i18n::MessageId::TuiPlaybackControlUnavailable)});
    }
  }

  void EventController::executeKeyAction(TuiKeyAction const action)
  {
    if (auto const optCommandAction = commandActionForKeyAction(action); optCommandAction)
    {
      runCommand(Command{.action = *optCommandAction});
      return;
    }

    using enum TuiKeyAction;

    switch (action)
    {
      case OpenCommandPalette:
      case OpenQuickFilter:
        cancelWorkspaceGestures();
        _shell.beginInput(action == OpenQuickFilter ? ShellInputMode::QuickFilter : ShellInputMode::Command);
        refreshCommandCompletion();
        break;
      case PreviousSection: _library.jumpToAdjacentSection(-1); break;
      case NextSection: _library.jumpToAdjacentSection(1); break;
      case SeekBackward: _seekViewModel.seekBy(-kKeyboardSeekDelta); break;
      case SeekForward: _seekViewModel.seekBy(kKeyboardSeekDelta); break;
      case VolumeDown: _volumeViewModel.adjustVolume(-kKeyboardVolumeDelta); break;
      case VolumeUp: _volumeViewModel.adjustVolume(kKeyboardVolumeDelta); break;
      case Quit:
      case ToggleListChooser:
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
      case PlaySelection:
      case PlaybackPlayPause:
      case PlaybackStop: AO_FATAL("Command-backed TUI key action was not mapped");
      case Count: break;
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
      case CommandAction::Play: playSelectedTrack(); break;
      case CommandAction::TogglePlayback: executePlaybackCommand(uimodel::PlaybackCommand::PlayPause); break;
      case CommandAction::Stop: executePlaybackCommand(uimodel::PlaybackCommand::Stop); break;
      case CommandAction::Quit: _requestExit(); break;
    }
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

    _shell.setCommandCompletion((*callback)(_shell.inputDraft()));
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
      { return waitForFilterDebounce(runtime, owner, generation, stopToken); },
      "TUI Quick-filter debounce");
  }

  void EventController::cancelFilterDebounce() noexcept
  {
    _filterDebounceTask.reset();
    ++_filterDebounceGeneration;
  }

  async::Task<void> EventController::waitForFilterDebounce(async::Runtime* const runtime,
                                                           EventController* const owner,
                                                           std::uint64_t const generation,
                                                           std::stop_token const stopToken)
  {
    co_await runtime->sleepFor(kFilterDebounceInterval, stopToken);
    co_await runtime->resumeOnCallbackExecutor(stopToken);
    owner->applyPendingFilter(generation);
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
      _shell.applyCommandCompletion();
    }

    _library.setFilterDraft(_shell.inputDraft());
    applyFilter();
    _shell.closeInput();
  }

  bool EventController::selectTrackFromScrollbar(std::int32_t const row)
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

  void EventController::cancelSeekInteraction()
  {
    if (!_optSeekRailDrag)
    {
      return;
    }

    if (_seekSlider.hasPendingFinalSeek())
    {
      _seekViewModel.seekFinal(_playback.snapshot().transport.elapsed);
    }

    _optSeekRailDrag.reset();
    _seekSlider.reset();
  }

  void EventController::cancelColumnResize()
  {
    _optTrackColumnResizeDrag.reset();
    _trackColumnResizePreview = {};
  }

  bool EventController::hasWorkspaceGesture() const noexcept
  {
    return _optSeekRailDrag || _optTrackScrollbarDrag || _optTrackColumnResizeDrag;
  }

  void EventController::cancelWorkspaceGestures()
  {
    cancelSeekInteraction();
    _optTrackScrollbarDrag.reset();
    cancelColumnResize();
  }

  void EventController::openOverlay(Overlay const overlay)
  {
    cancelWorkspaceGestures();
    _shell.openOverlay(overlay);
  }

  void EventController::closeOverlay()
  {
    cancelWorkspaceGestures();
    _shell.closeOverlay();
  }

  bool EventController::handleTrackColumnResizeDrag(ftxui::Mouse const& mouse)
  {
    if (mouse.motion != ftxui::Mouse::Moved && mouse.motion != ftxui::Mouse::Released)
    {
      cancelColumnResize();
      return false;
    }

    AO_EXPECTS(_optTrackColumnResizeDrag);
    auto const drag = *_optTrackColumnResizeDrag;

    if (_library.currentListId() != drag.listId)
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

  std::optional<bool> EventController::handleActiveMouseDrag(ftxui::Mouse const& mouse)
  {
    if (_optSeekRailDrag)
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
      return handleTrackColumnResizeDrag(mouse);
    }

    return std::nullopt;
  }

  std::optional<bool> EventController::handleMouseWheel(ftxui::Mouse const& mouse)
  {
    if ((mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) &&
        mouse.motion == ftxui::Mouse::Pressed)
    {
      if (!isModalOverlay(_shell.overlay()) && contains(_hitRegions.trackTableBox, mouse.x, mouse.y))
      {
        auto const delta =
          mouse.button == ftxui::Mouse::WheelUp ? -kMouseWheelSelectionDelta : kMouseWheelSelectionDelta;
        _library.moveFocusedSelection(false, delta);
        return true;
      }

      return false;
    }

    return std::nullopt;
  }

  bool EventController::handleMouseMove(ftxui::Mouse const& mouse)
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

    if (!_seekSlider.beginPointerInteraction())
    {
      return false;
    }

    _optSeekRailDrag = SeekRailDrag{};
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

    _trackColumnResizePreview = {};
    _optTrackColumnResizeDrag = TrackColumnResizeDrag{
      .field = handleIt->field,
      .startX = mouse.x,
      .startColumns = handleIt->columns,
      .listId = _library.currentListId(),
    };
    return true;
  }

  std::optional<bool> EventController::handleScrollbarPress(ftxui::Mouse const& mouse)
  {
    if (isModalOverlay(_shell.overlay()) || !containsTrackScrollbar(_hitRegions.trackTableBox, mouse.x, mouse.y))
    {
      return std::nullopt;
    }

    _optTrackScrollbarDrag = TrackScrollbarDrag{};

    if (!selectTrackFromScrollbar(mouse.y))
    {
      _optTrackScrollbarDrag.reset();
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

    if (hitRegionIt->sectionIndex < 0 ||
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
      toggleListChooser();
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

  bool EventController::handleOverlayPress(ftxui::Mouse const& mouse)
  {
    if (_shell.overlay() == Overlay::PresentationPanel)
    {
      auto const hitRegionIt = std::ranges::find_if(_hitRegions.presentationRows,
                                                    [&](PresentationRowHitRegion const& hitRegion)
                                                    { return contains(hitRegion.box, mouse.x, mouse.y); });

      if (hitRegionIt != _hitRegions.presentationRows.end())
      {
        if (_library.setSelectedPresentation(hitRegionIt->rowIndex))
        {
          selectPresentation();
          return true;
        }
      }

      return false;
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

      if (_outputDevices.selectRow(hitRegionIt->rowIndex))
      {
        closeOverlay();
      }

      return true;
    }

    return false;
  }

  bool EventController::handleMouse(ftxui::Mouse const& mouse)
  {
    auto const modalInputActive = _shell.isInputActive() || isModalOverlay(_shell.overlay());

    // A gesture aimed at the workspace cannot be finished across a surface that
    // took the workspace away, whichever of the two arrived first.
    if (modalInputActive && hasWorkspaceGesture())
    {
      cancelWorkspaceGestures();
      return false;
    }

    if (_shell.isInputActive() && mouse.motion != ftxui::Mouse::Moved)
    {
      return false;
    }

    if (auto const optHandled = handleActiveMouseDrag(mouse); optHandled)
    {
      return *optHandled;
    }

    if (auto const optHandled = handleMouseWheel(mouse); optHandled)
    {
      return *optHandled;
    }

    if (mouse.motion == ftxui::Mouse::Moved)
    {
      return handleMouseMove(mouse);
    }

    if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Pressed)
    {
      return false;
    }

    if (auto const optHandled = handleSeekRailPress(mouse, modalInputActive); optHandled)
    {
      return *optHandled;
    }

    if (auto const optHandled = handleColumnResizePress(mouse); optHandled)
    {
      return *optHandled;
    }

    if (auto const optHandled = handleScrollbarPress(mouse); optHandled)
    {
      return *optHandled;
    }

    if (auto const optHandled = handleSectionPress(mouse); optHandled)
    {
      return *optHandled;
    }

    if (auto const optHandled = handleButtonPress(mouse); optHandled)
    {
      return *optHandled;
    }

    return handleOverlayPress(mouse);
  }

  bool EventController::handleCommandEvent(ftxui::Event const& event)
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
      if (_shell.inputMode() == ShellInputMode::QuickFilter)
      {
        closeQuickFilter(true);
        return true;
      }

      auto const optCommand = parseCommand(_shell.inputDraft());

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

        return true;
      }

      _shell.closeInput();
      runCommand(*optCommand);
      return true;
    }

    if (event == ftxui::Event::Tab)
    {
      if (_shell.applyCommandCompletion())
      {
        refreshCommandCompletion();
        scheduleFilterDebounce();
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

    if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
    {
      _shell.moveCommandCompletionByPage(listNavigationDelta(event).value_or(0));
      return true;
    }

    if (event == ftxui::Event::Backspace)
    {
      _shell.backspaceInput();
      refreshCommandCompletion();
      scheduleFilterDebounce();
      return true;
    }

    if (event.is_character())
    {
      _shell.appendInputText(event.character());
      refreshCommandCompletion();
      scheduleFilterDebounce();
    }

    return true;
  }

  bool EventController::handleOverlayEvent(ftxui::Event const& event)
  {
    switch (_shell.overlay())
    {
      case Overlay::ListChooser:
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

        if (_keymapPlan.actionFor(event) == TuiKeyAction::ToggleListChooser)
        {
          toggleListChooser();
        }

        return true;
      case Overlay::DetailPanel:
        // Detail inspects whatever the table has selected, so every other key
        // belongs to the workspace it is watching.
        return false;
      case Overlay::QualityPanel:
        if (_keymapPlan.actionFor(event) == TuiKeyAction::ToggleAudioPipeline)
        {
          toggleQualityPanel();
        }

        return true;
      case Overlay::OutputDevices:
        if (handleListNavigation(event, [this](std::int32_t const delta) { _outputDevices.moveSelection(delta); }))
        {
          return true;
        }

        if (event == ftxui::Event::Return)
        {
          selectOutputDevice();
          return true;
        }

        if (_keymapPlan.actionFor(event) == TuiKeyAction::ToggleOutputDevices)
        {
          toggleOutputDevices();
        }

        return true;
      case Overlay::PresentationPanel:
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

        if (_keymapPlan.actionFor(event) == TuiKeyAction::TogglePresentations)
        {
          togglePresentationPanel();
        }

        return true;
      case Overlay::Notifications:
        if (event == ftxui::Event::Character("x"))
        {
          if (_activityStatusViewModel.viewState().compact.dismissible)
          {
            _activityStatusViewModel.dismissCompact();
          }

          return true;
        }

        if (_keymapPlan.actionFor(event) == TuiKeyAction::ToggleNotifications)
        {
          toggleNotificationCenter();
        }

        return true;
      case Overlay::Help: return true;
      case Overlay::None: return false;
    }

    return false;
  }

  bool EventController::handleRootEvent(ftxui::Event const& event)
  {
    if (handleListNavigation(event, [this](std::int32_t const delta) { _library.moveFocusedSelection(false, delta); }))
    {
      return true;
    }

    if (auto const optAction = _keymapPlan.actionFor(event); optAction)
    {
      executeKeyAction(*optAction);
      return true;
    }

    return false;
  }

  bool EventController::handleEvent(ftxui::Event const& event)
  {
    if (event == ftxui::Event::CtrlC)
    {
      _requestExit();
      return true;
    }

    if (event == ftxui::Event::Custom)
    {
      return false;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;
      return handleMouse(mouseEvent.mouse());
    }

    if (_shell.isInputActive())
    {
      return handleCommandEvent(event);
    }

    // Escape is protocol-owned before any overlay sees it, so rebinding cannot
    // strand the user inside a modal surface.
    if (event == ftxui::Event::Escape)
    {
      runCommand({.action = CommandAction::CloseOverlay});
      return true;
    }

    // A modal overlay answers everything, so reaching the workspace below means
    // the open overlay left this key alone.
    if (isOverlayActive(_shell.overlay()) && handleOverlayEvent(event))
    {
      return true;
    }

    return handleRootEvent(event);
  }

  void EventController::cancelTransientInteractions()
  {
    cancelFilterDebounce();
    cancelWorkspaceGestures();
  }
} // namespace ao::tui
