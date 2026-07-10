// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "App.h"

#include "AnchoredOverlay.h"
#include "AudioBackendBootstrap.h"
#include "CommandCompletionProvider.h"
#include "CommandPalettePanel.h"
#include "CoverArt.h"
#include "EventController.h"
#include "Executor.h"
#include "LibraryController.h"
#include "NotificationCenterPanel.h"
#include "OutputDeviceController.h"
#include "OutputDevicePanel.h"
#include "PlaybackPanel.h"
#include "PlaybackStatusFormatter.h"
#include "PresentationPanel.h"
#include "QualityPanel.h"
#include "Render.h"
#include "SelectionNavigation.h"
#include "ShellInteractionModel.h"
#include "SignalExitWatcher.h"
#include "StatusBar.h"
#include "Style.h"
#include "TrackPresentationNavigation.h"
#include "TrackTable.h"
#include "TuiHitRegions.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/seek/PlaybackTimeViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr auto kPlaybackTickInterval = std::chrono::milliseconds{250};
    constexpr std::int32_t kBlockCoverArtColumns = 24;
    constexpr std::int32_t kBlockCoverArtRows = 12;
    constexpr std::int32_t kKittyCoverArtColumns = 768;
    constexpr std::int32_t kKittyCoverArtRows = 384;
    constexpr std::int32_t kNotificationCenterPanelRows = 12;
    ftxui::Element commandPalettePopover(ShellInteractionModel const& shell,
                                         std::int32_t const terminalColumns,
                                         std::int32_t const terminalRows)
    {
      if (!shell.isCommandActive())
      {
        return {};
      }

      auto const panelColumns = commandPalettePanelColumns(terminalColumns);
      auto const panelRows = commandPalettePanelRows(terminalRows);

      return centerPopover(commandPalettePanel(shell, panelColumns) |
                           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, panelColumns) |
                           ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, panelRows));
    }

    ftxui::Element presentationPopover(ShellInteractionModel const& shell,
                                       LibraryController const& library,
                                       ftxui::Box const& presentationButtonBox,
                                       std::int32_t const terminalColumns,
                                       std::vector<PresentationRowHitRegion>* rowHitRegions)
    {
      if (shell.overlay() != Overlay::PresentationPanel)
      {
        return {};
      }

      auto const activePresentationId = library.activePresentationId();
      auto const panelColumns =
        presentationPanelColumns(library.presentationEntries(), activePresentationId, terminalColumns);

      return anchoredOverlay(presentationPanel(library.presentationEntries(),
                                               activePresentationId,
                                               library.selectedPresentation(),
                                               rowHitRegions,
                                               panelColumns) |
                               ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, kPresentationPanelRows),
                             presentationButtonBox,
                             AnchoredOverlayPlacement::Above,
                             AnchoredOverlaySize{.columns = panelColumns, .rows = kPresentationPanelRows},
                             AnchoredOverlayTerminal{.columns = terminalColumns});
    }

    ftxui::Element notificationPopover(ShellInteractionModel const& shell,
                                       uimodel::ActivityStatusViewState const& state,
                                       ftxui::Box const& activityStatusBox,
                                       std::int32_t const terminalColumns,
                                       std::int32_t const terminalRows,
                                       std::vector<NotificationDetailRowHitRegion>* rowHitRegions)
    {
      if (shell.overlay() != Overlay::Notifications)
      {
        return {};
      }

      auto const panelColumns = notificationCenterPanelColumns(state, terminalColumns);

      return anchoredOverlay(notificationCenterPanel(state, rowHitRegions, panelColumns) |
                               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, panelColumns) |
                               ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, kNotificationCenterPanelRows),
                             activityStatusBox,
                             AnchoredOverlayPlacement::Above,
                             AnchoredOverlaySize{.columns = panelColumns, .rows = kNotificationCenterPanelRows},
                             AnchoredOverlayTerminal{.columns = terminalColumns, .rows = terminalRows},
                             AnchoredOverlayOptions{.fallbackToBottom = true});
    }

    std::int32_t sidePanelColumnsLimit(std::int32_t const terminalColumns)
    {
      return terminalColumns <= 0 ? terminalColumns : std::max(1, terminalColumns / 2);
    }

    enum class CoverArtMode : std::uint8_t
    {
      Auto,
      Kitty,
      Blocks,
      Off,
    };

    CoverArtMode parseCoverArtMode(std::string const& value)
    {
      if (value == "kitty")
      {
        return CoverArtMode::Kitty;
      }

      if (value == "blocks")
      {
        return CoverArtMode::Blocks;
      }

      if (value == "off")
      {
        return CoverArtMode::Off;
      }

      return CoverArtMode::Auto;
    }

    bool supportsKittyGraphics()
    {
      auto const* term = std::getenv("TERM");
      auto const* termProgram = std::getenv("TERM_PROGRAM");

      return std::getenv("KITTY_WINDOW_ID") != nullptr || std::getenv("WEZTERM_EXECUTABLE") != nullptr ||
             (term != nullptr && std::string_view{term}.contains("xterm-kitty")) ||
             (termProgram != nullptr && std::string_view{termProgram} == "WezTerm");
    }

    bool shouldUseKittyCoverArt(CoverArtMode const mode)
    {
      if (mode == CoverArtMode::Kitty)
      {
        return true;
      }

      return mode == CoverArtMode::Auto && supportsKittyGraphics();
    }

    bool shouldUseBlockCoverArt(CoverArtMode const mode)
    {
      return mode == CoverArtMode::Blocks || (mode == CoverArtMode::Auto && !supportsKittyGraphics());
    }

    bool isValidBox(ftxui::Box const& box)
    {
      return box.x_max > box.x_min && box.y_max > box.y_min;
    }

    bool isSameBox(ftxui::Box const& left, ftxui::Box const& right)
    {
      return left.x_min == right.x_min && left.x_max == right.x_max && left.y_min == right.y_min &&
             left.y_max == right.y_max;
    }

    class PeriodicRefresh final
    {
    public:
      PeriodicRefresh(ftxui::ScreenInteractive& screen,
                      std::chrono::milliseconds interval,
                      std::function<bool()> shouldTick)
        : _screen{screen}, _interval{interval}, _shouldTick{std::move(shouldTick)}, _thread{[this] { run(); }}
      {
      }

      ~PeriodicRefresh()
      {
        _running.store(false);

        if (_thread.joinable())
        {
          _thread.join();
        }
      }

      PeriodicRefresh(PeriodicRefresh const&) = delete;
      PeriodicRefresh& operator=(PeriodicRefresh const&) = delete;
      PeriodicRefresh(PeriodicRefresh&&) = delete;
      PeriodicRefresh& operator=(PeriodicRefresh&&) = delete;

    private:
      void run()
      {
        while (_running.load())
        {
          std::this_thread::sleep_for(_interval);

          if (_running.load() && _shouldTick != nullptr && _shouldTick())
          {
            _screen.PostEvent(ftxui::Event::Custom);
          }
        }
      }

      ftxui::ScreenInteractive& _screen;
      std::chrono::milliseconds _interval;
      std::function<bool()> _shouldTick;
      std::atomic_bool _running{true};
      std::thread _thread;
    };

    std::optional<CoverArtRows> loadCoverArtPreview(rt::AppRuntime& runtime, ResourceId const resourceId)
    {
      if (resourceId == kInvalidResourceId)
      {
        return std::nullopt;
      }

      auto const reader = runtime.library().reader();
      auto optBytes = reader.loadResource(resourceId);

      if (!optBytes)
      {
        return std::nullopt;
      }

      return decodeCoverArtPreview(*optBytes, kBlockCoverArtColumns, kBlockCoverArtRows);
    }

    std::optional<std::vector<std::byte>> loadCoverArtPng(rt::AppRuntime& runtime, ResourceId const resourceId)
    {
      if (resourceId == kInvalidResourceId)
      {
        return std::nullopt;
      }

      auto const reader = runtime.library().reader();
      auto optBytes = reader.loadResource(resourceId);

      if (!optBytes)
      {
        return std::nullopt;
      }

      return decodeCoverArtPng(*optBytes, kKittyCoverArtColumns, kKittyCoverArtRows);
    }

    uimodel::FrameClock::TimePoint monotonicFrameTime()
    {
      auto const frameTime = std::chrono::steady_clock::now().time_since_epoch();
      auto const micros = std::chrono::duration_cast<uimodel::FrameClock::Duration>(frameTime).count();

      return uimodel::FrameClock::fromMicros(micros);
    }

    struct KittyPaintState final
    {
      bool visible = false;
      ResourceId paintedCoverArtId = kInvalidResourceId;
      ftxui::Box paintedCoverBox{};
    };

    bool isSameKittyImage(KittyPaintState const& state, ResourceId const coverArtId, ftxui::Box const& coverBox)
    {
      return state.visible && coverArtId == state.paintedCoverArtId && isSameBox(coverBox, state.paintedCoverBox);
    }

    void updateKittyCoverArt(KittyPaintState& state,
                             ShellInteractionModel const& shell,
                             ResourceId const cachedCoverArtId,
                             ftxui::Box const& coverBox,
                             std::optional<std::vector<std::byte>> const& optKittyCoverArtPng)
    {
      auto const shouldShow = shell.overlay() == Overlay::DetailPanel && optKittyCoverArtPng && isValidBox(coverBox);

      if (shouldShow)
      {
        if (state.visible && !isSameKittyImage(state, cachedCoverArtId, coverBox))
        {
          std::print("{}", kittyDeleteImageEscape(kKittyCoverArtImageId));
        }

        paintKittyCoverArt(coverBox, *optKittyCoverArtPng);
        state.paintedCoverArtId = cachedCoverArtId;
        state.paintedCoverBox = coverBox;
        state.visible = true;
        return;
      }

      if (!shouldShow && state.visible)
      {
        std::print("{}", kittyDeleteImageEscape(kKittyCoverArtImageId));
        std::fflush(stdout);
        state.visible = false;
        state.paintedCoverArtId = kInvalidResourceId;
        state.paintedCoverBox = {};
      }
    }
  } // namespace

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  std::int32_t run(AppOptions const& options)
  {
    auto const coverArtMode = parseCoverArtMode(options.coverArtMode);
    auto const kittyCoverArt = shouldUseKittyCoverArt(coverArtMode);
    auto const blockCoverArt = shouldUseBlockCoverArt(coverArtMode);

    std::filesystem::create_directories(options.configPath.parent_path());
    rt::Log::initialize(options.logLevel, options.libraryRoot / ".aobus" / "logs", rt::LogConsoleMode::Disabled);

    auto screen = ftxui::ScreenInteractive::FullscreenAlternateScreen();
    screen.TrackMouse(true);
    auto executorPtr = std::make_unique<Executor>(screen);
    auto* const executor = executorPtr.get();
    auto runtime = rt::AppRuntime{rt::AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = options.libraryRoot,
      .databasePath = options.databasePath,
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(options.configPath),
    }};

    registerPlatformAudioBackends(runtime);

    auto library = LibraryController{runtime};
    auto shell = ShellInteractionModel{};
    auto cachedCoverArtId = kInvalidResourceId;
    auto optCoverArtPreview = std::optional<CoverArtRows>{};
    auto optKittyCoverArtPng = std::optional<std::vector<std::byte>>{};
    auto hitRegions = TuiHitRegions{};
    auto trackColumnWidthOverrides = std::vector<TrackColumnWidthOverride>{};
    auto kittyPaintState = KittyPaintState{};

    auto& playback = runtime.playback();
    auto requestRefresh = [&screen] { screen.PostEvent(ftxui::Event::Custom); };
    auto clockTickActive = std::atomic_bool{shouldTickTransportClock(playback.state().transport)};
    auto activityAutoDismissActive = std::atomic_bool{false};
    auto playbackClock = uimodel::PlaybackPositionInterpolator{};
    auto optPreviewElapsed = std::optional<std::chrono::milliseconds>{};
    auto playbackTime =
      uimodel::PlaybackTimeViewModel{playback,
                                     [&](uimodel::PlaybackTimeViewState const& view)
                                     {
                                       clockTickActive.store(shouldTickTransportClock(playback.state().transport));

                                       if (view.duration == std::chrono::milliseconds{0})
                                       {
                                         optPreviewElapsed.reset();
                                         playbackClock.reset();
                                         requestRefresh();
                                         return;
                                       }

                                       if (view.isPreviewing)
                                       {
                                         optPreviewElapsed = view.elapsed;
                                         requestRefresh();
                                         return;
                                       }

                                       optPreviewElapsed.reset();
                                       playbackClock.updateState(view.elapsed, view.duration, view.isPlaying);
                                       requestRefresh();
                                     }};
    auto activityStatusViewModel = uimodel::ActivityStatusViewModel{
      runtime.notifications(),
      [&](uimodel::ActivityStatusViewState const& view)
      {
        activityAutoDismissActive.store(view.compact.optAutoDismissTimeout.has_value());
        requestRefresh();
      },
      uimodel::ActivityStatusViewModelOptions{.libraryChanges = &runtime.library().changes()}};
    runtime.notifications().post(rt::NotificationSeverity::Info, "Ready");

    auto nowPlayingSub = playback.onNowPlayingChanged([requestRefresh](auto const&) { requestRefresh(); });
    auto qualitySub = playback.onQualityChanged([requestRefresh](auto const&) { requestRefresh(); });
    auto volumeSub = playback.onVolumeChanged([requestRefresh](float) { requestRefresh(); });
    auto mutedSub = playback.onMutedChanged([requestRefresh](bool) { requestRefresh(); });
    auto outputDevices = OutputDeviceController{playback, requestRefresh};
    auto commandCompletions = CommandCompletionProvider{library, runtime.completion(), runtime.workspace()};
    auto events = EventController{screen,
                                  shell,
                                  library,
                                  playback,
                                  EventControllerBindings{
                                    .outputDevices = &outputDevices,
                                    .hitRegions = &hitRegions,
                                    .trackColumnWidthOverrides = &trackColumnWidthOverrides,
                                    .activityStatusViewModel = &activityStatusViewModel,
                                    .notifications = &runtime.notifications(),
                                    .commandCompletionCallback = [&commandCompletions](std::string_view const draft)
                                    { return commandCompletions.complete(draft); },
                                  }};

    auto rendererPtr = ftxui::Renderer(
      [&]
      {
        using namespace ftxui;

        auto const selectedTrackView = library.selectedTrackView();
        auto const selectedCoverArtId = selectedTrackView.coverArtId;

        auto const detailVisible = shell.overlay() == Overlay::DetailPanel;

        if (detailVisible && selectedCoverArtId != cachedCoverArtId)
        {
          cachedCoverArtId = selectedCoverArtId;
          optCoverArtPreview = blockCoverArt ? loadCoverArtPreview(runtime, cachedCoverArtId) : std::nullopt;
          optKittyCoverArtPng = kittyCoverArt ? loadCoverArtPng(runtime, cachedCoverArtId) : std::nullopt;
        }

        if (!detailVisible)
        {
          hitRegions.coverBox = ftxui::Box{};
        }

        auto coverElementPtr = kittyCoverArt ? renderKittyCoverArtPlaceholder(optKittyCoverArtPng != std::nullopt) |
                                                 reflect(hitRegions.coverBox)
                                             : renderCoverArtPreview(optCoverArtPreview) | reflect(hitRegions.coverBox);

        auto const currentListTitle = library.currentListTitle();
        auto const state = playback.state();
        hitRegions.clearFrameLocalRows();
        auto const frameTime = monotonicFrameTime();
        auto const displayElapsed = optPreviewElapsed.value_or(playbackClock.interpolateElapsed(frameTime));
        auto const animationElapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(frameTime.time_since_epoch());
        auto const viewState = runtime.views().trackListState(library.activeViewId());
        auto const terminalSize = ftxui::Terminal::Size();
        auto const terminalColumns = terminalSize.dimx;
        auto const terminalRows = terminalSize.dimy;
        auto const playbackRows = playbackBarRows(terminalRows);
        auto const hoveredButton = shell.isCommandActive() ? HoveredButton::None : events.hoveredButton();
        auto tableElementPtr =
          trackTableView(library.tracks(),
                         library.sections(),
                         library.selectedTrack(),
                         state.nowPlaying.trackId,
                         viewState.presentation,
                         TrackTableViewOptions{.columnWidths = &trackColumnWidthOverrides,
                                               .resizeHandles = &hitRegions.trackColumnResizeHandles,
                                               .sectionRowHitRegions = &hitRegions.trackSectionRows,
                                               .tableBox = &hitRegions.trackTableBox,
                                               .availableColumns = std::max(1, terminalColumns - 2)});
        auto const presentationTitle = trackPresentationDisplayId(viewState.presentation.id);
        auto workspaceElementPtr =
          style::titledPanel(
            "",
            std::move(tableElementPtr),
            style::PanelOptions{
              .leftFooter = style::PanelEdgeButton{.label = "list",
                                                   .value = currentListTitle,
                                                   .box = &hitRegions.libraryButtonBox,
                                                   .hovered = hoveredButton == HoveredButton::Library},
              .leftFooterRight = style::PanelEdgeButton{.label = "view",
                                                        .value = presentationTitle,
                                                        .box = &hitRegions.presentationButtonBox,
                                                        .hovered = hoveredButton == HoveredButton::Presentation},
              .rightFooter = selectionSummary(library.tracks().size(), library.selectedTrack())}) |
          flex;
        auto mainContentPtr = workspaceElementPtr;
        auto popoverElementPtr = ftxui::Element{};
        auto mainLayerPopover = [&](ftxui::Box const& rootAnchor,
                                    AnchoredOverlayPlacement const placement,
                                    std::int32_t const columns,
                                    std::int32_t const rows,
                                    ftxui::Element contentPtr)
        {
          return anchoredOverlay(std::move(contentPtr),
                                 rootAnchor,
                                 placement,
                                 AnchoredOverlaySize{.columns = columns, .rows = rows},
                                 AnchoredOverlayTerminal{.columns = terminalColumns, .rows = terminalRows},
                                 AnchoredOverlayOptions{.overlayLayerTopRows = playbackRows});
        };

        switch (shell.overlay())
        {
          case Overlay::None: break;
          case Overlay::ListChooser:
          {
            auto const panelColumns = libraryChooserPaneColumns(library.libraryLabels(), terminalColumns);
            auto const panelRows =
              static_cast<std::int32_t>(std::max<std::size_t>(1, library.libraryLabels().size())) + 4;
            popoverElementPtr =
              mainLayerPopover(hitRegions.libraryButtonBox,
                               AnchoredOverlayPlacement::Above,
                               panelColumns,
                               panelRows,
                               libraryChooserPane(library.libraryLabels(), library.selectedList(), panelColumns));
            break;
          }
          case Overlay::DetailPanel:
          {
            auto const panelColumns =
              detailPaneColumns(selectedTrackView.track, sidePanelColumnsLimit(terminalColumns));
            mainContentPtr = hbox({
              workspaceElementPtr,
              detailPane(selectedTrackView.track, std::move(coverElementPtr), panelColumns),
            });
            break;
          }
          case Overlay::QualityPanel:
          {
            auto const panelColumns = qualityPanelColumns(state, terminalColumns);
            popoverElementPtr = mainLayerPopover(hitRegions.soulButtonBox,
                                                 AnchoredOverlayPlacement::Below,
                                                 panelColumns,
                                                 0,
                                                 qualityPanel(state, panelColumns));
            break;
          }
          case Overlay::OutputDevices:
          {
            auto const panelColumns = outputDevicePanelColumns(outputDevices.viewState(), terminalColumns);
            popoverElementPtr = mainLayerPopover(
              hitRegions.outputDeviceButtonBox,
              AnchoredOverlayPlacement::Below,
              panelColumns,
              0,
              outputDevicePanel(
                outputDevices.viewState(), outputDevices.selectedRow(), &hitRegions.outputDeviceRows, panelColumns));
            break;
          }
          case Overlay::PresentationPanel:
          case Overlay::Notifications: break;
          case Overlay::Help:
          {
            auto const panelColumns = helpPaneColumns(sidePanelColumnsLimit(terminalColumns));
            mainContentPtr = hbox({
              workspaceElementPtr,
              helpPane(panelColumns),
            });
            break;
          }
        }

        if (!shell.isCommandActive() && shell.overlay() == Overlay::None && popoverElementPtr == nullptr &&
            events.isQualityHoverVisible())
        {
          auto const panelColumns = qualityPanelColumns(state, terminalColumns);
          popoverElementPtr = mainLayerPopover(hitRegions.soulButtonBox,
                                               AnchoredOverlayPlacement::Below,
                                               panelColumns,
                                               0,
                                               qualityPanel(state, panelColumns));
        }

        auto mainLayerPtr = popoverElementPtr == nullptr ? std::move(mainContentPtr)
                                                         : dbox({
                                                             std::move(mainContentPtr),
                                                             std::move(popoverElementPtr),
                                                           });

        auto rootPtr = vbox({
          playbackBar(PlaybackBarViewState{.playbackState = &state,
                                           .displayElapsed = displayElapsed,
                                           .animationElapsed = animationElapsed,
                                           .outputView = &outputDevices.viewState(),
                                           .outputDeviceBox = &hitRegions.outputDeviceButtonBox,
                                           .soulButtonBox = &hitRegions.soulButtonBox,
                                           .seekRailBox = &hitRegions.seekRailBox,
                                           .outputDeviceHovered = hoveredButton == HoveredButton::OutputDevice,
                                           .terminalColumns = terminalColumns}),
          std::move(mainLayerPtr) | flex,
          statusBar(StatusBarViewState{.activityStatus = &activityStatusViewModel.viewState(),
                                       .terminalColumns = terminalColumns,
                                       .filterDraft = library.filterDraft(),
                                       .shell = &shell,
                                       .activityStatusBox = &hitRegions.activityStatusBox,
                                       .activityStatusHovered = hoveredButton == HoveredButton::ActivityStatus}),
        });

        if (auto commandPopoverPtr = commandPalettePopover(shell, terminalColumns, terminalRows);
            commandPopoverPtr != nullptr)
        {
          return dbox({
            std::move(rootPtr),
            std::move(commandPopoverPtr),
          });
        }

        if (auto presentationPopoverPtr = presentationPopover(
              shell, library, hitRegions.presentationButtonBox, terminalColumns, &hitRegions.presentationRows);
            presentationPopoverPtr != nullptr)
        {
          return dbox({
            std::move(rootPtr),
            std::move(presentationPopoverPtr),
          });
        }

        if (auto notificationPopoverPtr = notificationPopover(shell,
                                                              activityStatusViewModel.viewState(),
                                                              hitRegions.activityStatusBox,
                                                              terminalColumns,
                                                              terminalRows,
                                                              &hitRegions.notificationDetailRows);
            notificationPopoverPtr != nullptr)
        {
          return dbox({
            std::move(rootPtr),
            std::move(notificationPopoverPtr),
          });
        }

        return rootPtr;
      });

    auto componentPtr =
      ftxui::CatchEvent(rendererPtr, [&](ftxui::Event const& event) { return events.handleEvent(event); });

    auto loop = ftxui::Loop{&screen, componentPtr};
    auto signalExit = SignalExitWatcher{[&screen] { screen.Post(screen.ExitLoopClosure()); }};
    auto refreshTick = PeriodicRefresh{screen,
                                       kPlaybackTickInterval,
                                       [&clockTickActive, &activityAutoDismissActive]
                                       { return clockTickActive.load() || activityAutoDismissActive.load(); }};

    executor->drainPendingTasks();

    while (!loop.HasQuitted())
    {
      loop.RunOnceBlocking();

      activityStatusViewModel.expireTransientIfDue();

      if (kittyCoverArt)
      {
        updateKittyCoverArt(kittyPaintState, shell, cachedCoverArtId, hitRegions.coverBox, optKittyCoverArtPng);
      }
    }

    if (kittyCoverArt && kittyPaintState.visible)
    {
      std::print("{}", kittyDeleteImageEscape(kKittyCoverArtImageId));
      std::fflush(stdout);
    }

    playback.stop();
    runtime.async().requestStop();
    runtime.async().join();
    rt::Log::shutdown();
    return 0;
  }
} // namespace ao::tui
