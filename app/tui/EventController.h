// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibraryController.h"
#include "OutputDeviceController.h"
#include "ShellInteractionModel.h"
#include "TuiHitRegions.h"
#include "TuiKeymap.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class NotificationService;
  class PlaybackService;
}

namespace ao::tui
{
  class LibraryScanController;

  using InputCompletionCallback = std::function<std::optional<rt::CompletionResult>(std::string_view draft)>;

  struct TrackColumnResizePreview final
  {
    ListId listId = kInvalidListId;
    std::vector<uimodel::TrackColumnState> layout{};
  };

  /**
   * The collaborators an EventController drives. Every field is mandatory; the
   * aggregate exists because they do not read as positional constructor
   * arguments, not to make any of them optional.
   */
  struct EventControllerBindings final
  {
    OutputDeviceController& outputDevices;
    TuiHitRegions& hitRegions;
    uimodel::TrackColumnLayouts& trackColumnLayouts;
    TrackColumnResizePreview& trackColumnResizePreview;
    uimodel::ActivityStatusViewModel& activityStatusViewModel;
    rt::NotificationService& notifications;
    LibraryScanController& libraryScan;
    std::function<void()> requestExit;
    InputCompletionCallback commandCompletionCallback;
    InputCompletionCallback filterCompletionCallback;
  };

  class EventController final
  {
  public:
    EventController(ShellInteractionModel& shell,
                    LibraryController& library,
                    async::Runtime& asyncRuntime,
                    rt::PlaybackService& playback,
                    TuiKeymapPlan const& keymapPlan,
                    EventControllerBindings bindings);

    bool isQualityHoverVisible() const noexcept { return _qualityHoverVisible; }
    HoveredButton hoveredButton() const noexcept { return _hoveredButton; }
    bool handleEvent(ftxui::Event const& event);
    void cancelTransientInteractions();

  private:
    void openSelectedList();
    void reloadActiveList();
    void applyFilter(bool reportError = true);
    void toggleListChooser();
    void toggleDetailPanel();
    void toggleQualityPanel();
    void toggleOutputDevices();
    void togglePresentationPanel();
    void toggleNotificationCenter();
    void selectOutputDevice();
    void selectPresentation();
    void revealCurrentTrack();
    void playSelectedTrack();
    void executePlaybackCommand(uimodel::PlaybackCommand command);
    void executeKeyAction(TuiKeyAction action);
    void runCommand(Command const& command);
    void postActivityNotification(rt::NotificationSeverity severity, std::string message);
    void refreshCommandCompletion();
    void scheduleFilterDebounce();
    void cancelFilterDebounce() noexcept;
    void applyPendingFilter(std::uint64_t generation);
    void closeQuickFilter(bool acceptCompletion);
    static async::Task<void> waitForFilterDebounce(async::Runtime* runtime,
                                                   EventController* owner,
                                                   std::uint64_t generation,
                                                   std::stop_token stopToken);
    bool handleMouse(ftxui::Mouse const& mouse);
    std::optional<bool> handleActiveMouseDrag(ftxui::Mouse const& mouse);
    bool handleTrackColumnResizeDrag(ftxui::Mouse const& mouse);
    std::optional<bool> handleMouseWheel(ftxui::Mouse const& mouse);
    bool handleMouseMove(ftxui::Mouse const& mouse);
    std::optional<bool> handleSeekRailPress(ftxui::Mouse const& mouse, bool modalInputActive);
    std::optional<bool> handleColumnResizePress(ftxui::Mouse const& mouse);
    std::optional<bool> handleScrollbarPress(ftxui::Mouse const& mouse);
    std::optional<bool> handleSectionPress(ftxui::Mouse const& mouse);
    std::optional<bool> handleButtonPress(ftxui::Mouse const& mouse);
    bool handleOverlayPress(ftxui::Mouse const& mouse);
    bool handleCommandEvent(ftxui::Event const& event);
    bool handleOverlayEvent(ftxui::Event const& event);
    bool handleRootEvent(ftxui::Event const& event);
    bool selectTrackFromScrollbar(std::int32_t row);
    void syncSeekSlider();
    std::chrono::milliseconds seekRailElapsed(std::int32_t column) const;
    void applySeekUpdate(uimodel::SeekSliderUpdate const& update);
    void cancelSeekInteraction();
    void cancelColumnResize();
    bool hasWorkspaceGesture() const noexcept;
    /**
     * @brief Drops pointer gestures the workspace can no longer own.
     *
     * Seek, scrollbar, and column drags are all aimed at geometry that an
     * overlay or text input either moves or takes away, so a surface change
     * ends them rather than letting them finish against a layout the user
     * never aimed at.
     */
    void cancelWorkspaceGestures();
    /// Opens @p overlay, retiring gestures the change invalidates.
    void openOverlay(Overlay overlay);
    /// Closes the active overlay, retiring gestures the change invalidates.
    void closeOverlay();

    struct TrackColumnResizeDrag final
    {
      rt::TrackField field = rt::TrackField::Title;
      std::int32_t startX = 0;
      std::int32_t startColumns = 0;
      ListId listId = kInvalidListId;
    };

    struct TrackScrollbarDrag final
    {};

    struct SeekRailDrag final
    {};

    ShellInteractionModel& _shell;
    LibraryController& _library;
    TuiKeymapPlan const& _keymapPlan;
    async::Runtime& _asyncRuntime;
    rt::PlaybackService& _playback;
    uimodel::PlaybackActions _playbackActions;
    uimodel::PlaybackPositionViewModel _seekViewModel;
    uimodel::VolumeViewModel _volumeViewModel;
    OutputDeviceController& _outputDevices;
    TuiHitRegions& _hitRegions;
    uimodel::TrackColumnLayouts& _trackColumnLayouts;
    TrackColumnResizePreview& _trackColumnResizePreview;
    std::optional<TrackColumnResizeDrag> _optTrackColumnResizeDrag{};
    std::optional<TrackScrollbarDrag> _optTrackScrollbarDrag{};
    std::optional<SeekRailDrag> _optSeekRailDrag{};
    uimodel::SeekInteraction _seekSlider{};
    uimodel::ActivityStatusViewModel& _activityStatusViewModel;
    rt::NotificationService& _notifications;
    LibraryScanController& _libraryScan;
    std::function<void()> _requestExit;
    InputCompletionCallback _commandCompletionCallback;
    InputCompletionCallback _filterCompletionCallback;
    bool _qualityHoverVisible = false;
    HoveredButton _hoveredButton = HoveredButton::None;
    std::uint64_t _filterDebounceGeneration = 0;
    // Declared last so teardown requests stop before any callback target is destroyed.
    async::TaskHandle _filterDebounceTask{};
  };
} // namespace ao::tui
