// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibraryController.h"
#include "OutputDeviceController.h"
#include "ShellInteractionModel.h"
#include "TrackTable.h"
#include "TuiHitRegions.h"
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>
#include <ao/uimodel/playback/seek/SeekSliderInteractionModel.h>
#include <ao/uimodel/playback/seek/SeekViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
  class NotificationService;
  class PlaybackService;
}

namespace ao::tui
{
  using CommandCompletionCallback = std::function<std::optional<rt::CompletionResult>(std::string_view draft)>;

  struct EventControllerBindings final
  {
    OutputDeviceController* outputDevices = nullptr;
    TuiHitRegions* hitRegions = nullptr;
    std::vector<TrackColumnWidthOverride>* trackColumnWidthOverrides = nullptr;
    uimodel::ActivityStatusViewModel* activityStatusViewModel = nullptr;
    rt::NotificationService* notifications = nullptr;
    CommandCompletionCallback commandCompletionCallback{};
  };

  class EventController final
  {
  public:
    EventController(ftxui::ScreenInteractive& screen,
                    ShellInteractionModel& shell,
                    LibraryController& library,
                    rt::AppRuntime& runtime,
                    EventControllerBindings bindings = {});

    bool isQualityHoverVisible() const noexcept { return _qualityHoverVisible; }
    HoveredButton hoveredButton() const noexcept { return _hoveredButton; }
    bool handleEvent(ftxui::Event const& event);

  private:
    void openSelectedList();
    void reloadActiveList();
    void applyFilter();
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
    void runCommand(Command const& command);
    void postActivityNotification(rt::NotificationSeverity severity, std::string message);
    void refreshCommandCompletion();
    bool handleMouse(ftxui::Mouse const& mouse);
    std::optional<bool> handleActiveMouseDrag(ftxui::Mouse const& mouse);
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

    struct TrackColumnResizeDrag final
    {
      rt::TrackField field = rt::TrackField::Title;
      std::int32_t startX = 0;
      std::int32_t startColumns = 0;
    };

    struct TrackScrollbarDrag final
    {};

    struct SeekRailDrag final
    {};

    ftxui::ScreenInteractive& _screen;
    ShellInteractionModel& _shell;
    LibraryController& _library;
    rt::PlaybackService& _playback;
    uimodel::PlaybackCommandSurface _playbackCommands;
    uimodel::SeekViewModel _seekViewModel;
    uimodel::VolumeViewModel _volumeViewModel;
    OutputDeviceController* _outputDevices = nullptr;
    TuiHitRegions* _hitRegions = nullptr;
    std::vector<TrackColumnWidthOverride>* _trackColumnWidthOverrides = nullptr;
    std::optional<TrackColumnResizeDrag> _optTrackColumnResizeDrag{};
    std::optional<TrackScrollbarDrag> _optTrackScrollbarDrag{};
    std::optional<SeekRailDrag> _optSeekRailDrag{};
    uimodel::SeekSliderInteractionModel _seekSlider{};
    uimodel::ActivityStatusViewModel* _activityStatusViewModel = nullptr;
    rt::NotificationService* _notifications = nullptr;
    CommandCompletionCallback _commandCompletionCallback{};
    bool _qualityHoverVisible = false;
    HoveredButton _hoveredButton = HoveredButton::None;
  };
} // namespace ao::tui
