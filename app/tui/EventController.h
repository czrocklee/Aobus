// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Command.h"
#include "GoToMenu.h"
#include "HitRegions.h"
#include "Keymap.h"
#include "LibraryController.h"
#include "MouseBindings.h"
#include "NavigationPanel.h"
#include "OutputDeviceController.h"
#include "PanelResize.h"
#include "PanelWidths.h"
#include "Preferences.h"
#include "ShellInteractionModel.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
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
#include <ftxui/screen/box.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::rt
{
  class NotificationService;
  class PlaybackService;
}

namespace ao::tui
{
  class LibraryScanController;
  class SettingsEditor;
  class TrackEditController;

  using InputCompletionCallback =
    std::function<std::optional<rt::CompletionResult>(std::string_view draft, std::size_t cursor)>;

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
    HitRegions& hitRegions;
    uimodel::TrackColumnLayouts& trackColumnLayouts;
    TrackColumnResizePreview& trackColumnResizePreview;
    uimodel::ActivityStatusViewModel& activityStatusViewModel;
    rt::NotificationService& notifications;
    LibraryScanController& libraryScan;
    TrackEditController& trackEdit;
    SettingsEditor& settings;
    Preferences const& preferences;
    std::function<void()> requestExit;
    /// Whether the shell is holding input while a submitted write settles.
    std::function<bool()> isExitWaiting{};
    InputCompletionCallback commandCompletionCallback;
    InputCompletionCallback filterCompletionCallback;
    std::function<void()> requestLayoutCheckpoint{};
  };

  class EventController final
  {
  public:
    EventController(ShellInteractionModel& shell,
                    LibraryController& library,
                    async::Runtime& asyncRuntime,
                    rt::PlaybackService& playback,
                    KeymapPlan const& keymapPlan,
                    EventControllerBindings bindings);

    bool isQualityHoverVisible() const noexcept { return _preferences.qualityHover && _qualityHoverVisible; }
    PanelWidths panelWidths() const noexcept;
    bool isPanelResizing(HoveredButton panel) const noexcept;
    std::optional<PanelDivider> keyboardResizeDivider() const noexcept;
    HoveredButton hoveredButton() const noexcept { return _hoveredButton; }
    GoToMenuState goToMenuState() const;
    bool tryHandleEvent(ftxui::Event const& event);
    void cancelTransientInteractions();
    void syncWorkspaceGeometry();

  private:
    bool tryHandleDetailEvent(ftxui::Event const& event);
    void scrollDetail(std::int32_t delta);
    bool tryHandleNavigationEvent(ftxui::Event const& event);
    std::optional<bool> tryHandleNavigationMouse(ftxui::Mouse const& mouse);
    void activateNavigation(bool keyboard);
    void handleNavigationPress(ftxui::Mouse const& mouse);
    void leaveNavigation();
    void switchWorkspaceFocus();
    void reportNavigationPinChange(bool previous);
    void selectNavigationFromScrollbar(std::int32_t row);
    void reloadActiveList();
    void applyFilter(bool reportError = true);
    void toggleLists();
    void togglePinnedLists();
    void toggleDetailPanel();
    std::optional<bool> tryHandleDetailMouse(ftxui::Mouse const& mouse);
    void toggleQualityPanel();
    void toggleOutputDevices();
    void togglePresentationPanel();
    void toggleNotificationCenter();
    void editSelectedTrackProperties();
    void selectOutputDevice();
    void selectPresentation();
    void revealCurrentTrack();
    void activateGoTo(CommandAction action);
    bool tryHandleGoToEvent(ftxui::Event const& event);
    void navigateCurrentMetadata(bool album);
    void playSelectedTrack();
    void executePlaybackCommand(uimodel::PlaybackCommand command);
    void executeKeyAction(KeyAction action);
    void runCommand(Command const& command);
    void postActivityNotification(rt::NotificationSeverity severity, std::string message);
    void refreshCommandCompletion();
    void scheduleFilterDebounce();
    void cancelFilterDebounce() noexcept;
    void applyPendingFilter(std::uint64_t generation);
    void closeQuickFilter(bool acceptCompletion);
    static async::Task<void> waitForFilterDebounceAsync(async::Runtime* runtime,
                                                        EventController* owner,
                                                        std::uint64_t generation,
                                                        std::stop_token stopToken);
    bool tryHandleMouse(ftxui::Mouse const& mouse);
    bool tryHandleInputMouse(ftxui::Mouse const& mouse);
    bool tryHandleTrackPress(ftxui::Mouse const& mouse);
    std::optional<bool> handleActiveMouseDrag(ftxui::Mouse const& mouse);
    bool tryHandleTrackColumnResizeDrag(ftxui::Mouse const& mouse);
    std::optional<bool> handleMouseWheel(ftxui::Mouse const& mouse);
    bool tryHandleMouseMove(ftxui::Mouse const& mouse);
    std::optional<bool> handleSeekRailPress(ftxui::Mouse const& mouse, bool modalInputActive);
    std::optional<bool> handleColumnResizePress(ftxui::Mouse const& mouse);
    std::optional<bool> handleScrollbarPress(ftxui::Mouse const& mouse);
    std::optional<bool> handleSectionPress(ftxui::Mouse const& mouse);
    bool tryHandlePlaybackMetadataPress(ftxui::Mouse const& mouse);
    std::optional<bool> handleButtonPress(ftxui::Mouse const& mouse);
    bool tryHandleLibraryPress(ftxui::Mouse const& mouse);
    void selectLibraryList();
    bool tryHandlePresentationPress(ftxui::Mouse const& mouse);
    bool tryHandleGoToPress(ftxui::Mouse const& mouse, GoToMenuHitRegions const& hitRegions);
    bool tryHandleOverlayPress(ftxui::Mouse const& mouse);
    void submitCommandInput();
    bool tryHandleCommandEvent(ftxui::Event const& event);
    bool tryHandleListSearchEvent(ftxui::Event const& event);
    bool tryMoveOverlaySelection(std::int32_t delta);
    bool tryHandleOverlayNavigation(ftxui::Event const& event);
    bool tryHandleOverlayActivation(ftxui::Event const& event);
    bool tryHandleOverlayEvent(ftxui::Event const& event);
    bool tryHandleRootEvent(ftxui::Event const& event);
    bool trySelectTrackFromScrollbar(std::int32_t row);
    void syncSeekSlider();
    std::chrono::milliseconds seekRailElapsed(std::int32_t column) const;
    void applySeekUpdate(uimodel::SeekSliderUpdate const& update);
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

    NavigationGeometry panelGeometry() const;
    void beginKeyboardPanelResize();
    bool tryHandleKeyboardPanelResize(ftxui::Event const& event);
    void finishPanelResize(bool apply);
    bool tryBeginPanelResize(ftxui::Mouse const& mouse);
    bool tryHandlePanelResizeKey(ftxui::Event const& event);
    bool tryHandlePanelResizeDrag(ftxui::Mouse const& mouse);
    void commitPanelWidths(PanelWidths widths);

    struct PanelResizeInteraction final
    {
      PanelResize resize;
      std::optional<std::int32_t> optPointerStartX{};
      std::int32_t terminalColumns = 0;
      bool navigationPinned = false;
      bool detailVisible = false;
      PanelWidths preview{};
    };

    struct TrackColumnResizeDrag final
    {
      rt::TrackField field = rt::TrackField::Title;
      std::int32_t startX = 0;
      std::int32_t startColumns = 0;
      ListId listId = kInvalidListId;
      std::uint64_t rowsRevision = 0;
    };

    struct TrackScrollbarDrag final
    {};

    struct SeekRailDrag final
    {};

    ShellInteractionModel& _shell;
    LibraryController& _library;
    KeymapPlan const& _keymapPlan;
    async::Runtime& _asyncRuntime;
    rt::PlaybackService& _playback;
    uimodel::PlaybackActions _playbackActions;
    uimodel::PlaybackPositionViewModel _seekViewModel;
    uimodel::VolumeViewModel _volumeViewModel;
    OutputDeviceController& _outputDevices;
    HitRegions& _hitRegions;
    uimodel::TrackColumnLayouts& _trackColumnLayouts;
    TrackColumnResizePreview& _trackColumnResizePreview;
    std::variant<std::monostate, TrackColumnResizeDrag, TrackScrollbarDrag, SeekRailDrag, PanelResizeInteraction>
      _workspaceGesture{};
    uimodel::SeekInteraction _seekSlider{};
    uimodel::ActivityStatusViewModel& _activityStatusViewModel;
    rt::NotificationService& _notifications;
    LibraryScanController& _libraryScan;
    TrackEditController& _trackEdit;
    SettingsEditor& _settings;
    Preferences const& _preferences;
    std::function<void()> _requestExit;
    std::function<bool()> _isExitWaiting;
    InputCompletionCallback _commandCompletionCallback;
    InputCompletionCallback _filterCompletionCallback;
    std::function<void()> _requestLayoutCheckpoint;
    NavigationGeometry _lastNavigationGeometry{};
    ftxui::Box _lastTrackTableBox = kEmptyMouseBox;
    bool _navigationScrollbarDrag = false;
    TrackId _lastClickedTrack = kInvalidTrackId;
    ListId _lastClickedList = kInvalidListId;
    std::chrono::steady_clock::time_point _lastTrackClickTime{};
    bool _qualityHoverVisible = false;
    HoveredButton _hoveredButton = HoveredButton::None;
    std::uint64_t _filterDebounceGeneration = 0;
    async::Subscription _revealSub;
    // Declared last so teardown requests stop before any callback target is destroyed.
    async::TaskHandle _filterDebounceTask{};
  };
} // namespace ao::tui
