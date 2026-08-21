// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "App.h"

#include "AnchoredOverlay.h"
#include "AudioBackendBootstrap.h"
#include "CommandCompletionProvider.h"
#include "CommandPalettePanel.h"
#include "CoverArt.h"
#include "CoverArtLoader.h"
#include "EventController.h"
#include "Executor.h"
#include "FrameTimer.h"
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
#include "TuiTextCatalog.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/AppStateStore.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceSelectionPolicy.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/seek/PlaybackPositionViewModel.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>
#include <ao/utility/PlatformDirectories.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/terminal.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr auto kPlaybackTickInterval = std::chrono::milliseconds{250};
    constexpr std::int32_t kNotificationCenterPanelRows = 12;

    ftxui::Element commandPalettePopover(uimodel::PresentationTextCatalog const& textCatalog,
                                         TuiTextCatalog const& tuiTextCatalog,
                                         ShellInteractionModel const& shell,
                                         std::int32_t const terminalColumns,
                                         std::int32_t const terminalRows)
    {
      if (!shell.isCommandActive())
      {
        return {};
      }

      auto const panelColumns = commandPalettePanelColumns(terminalColumns);
      auto const panelRows = commandPalettePanelRows(terminalRows);

      return centerPopover(commandPalettePanel(textCatalog, tuiTextCatalog, shell, panelColumns) |
                           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, panelColumns) |
                           ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, panelRows));
    }

    ftxui::Element presentationPopover(uimodel::PresentationTextCatalog const& textCatalog,
                                       TuiTextCatalog const& tuiTextCatalog,
                                       ShellInteractionModel const& shell,
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
      auto const panelColumns = presentationPanelColumns(
        textCatalog, tuiTextCatalog, library.presentationEntries(), activePresentationId, terminalColumns);

      return anchoredOverlay(presentationPanel(textCatalog,
                                               tuiTextCatalog,
                                               library.presentationEntries(),
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

    ftxui::Element notificationPopover(uimodel::PresentationTextCatalog const& textCatalog,
                                       TuiTextCatalog const& tuiTextCatalog,
                                       ShellInteractionModel const& shell,
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

      auto const panelColumns = notificationCenterPanelColumns(textCatalog, tuiTextCatalog, state, terminalColumns);

      return anchoredOverlay(notificationCenterPanel(textCatalog, tuiTextCatalog, state, rowHitRegions, panelColumns) |
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
        : _screen{screen}
        , _interval{interval}
        , _shouldTick{std::move(shouldTick)}
        , _thread{[this]
                  {
                    try
                    {
                      run();
                    }
                    catch (...)
                    {
                      AO_FATAL_EXCEPTION(std::current_exception(), "TUI periodic-refresh thread");
                    }
                  }}
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

    struct AppFrameRenderer final
    {
      FrameTimer& frameTimer;
      uimodel::PresentationTextCatalog const& textCatalog;
      TuiTextCatalog const& tuiTextCatalog;
      LibraryController& library;
      ShellInteractionModel& shell;
      rt::AppRuntime& runtime;
      rt::PlaybackService& playback;
      OutputDeviceController& outputDevices;
      uimodel::ActivityStatusViewModel& activityStatusViewModel;
      EventController& events;
      TuiHitRegions& hitRegions;
      std::vector<TrackColumnWidthOverride>& trackColumnWidthOverrides;
      uimodel::PlaybackPositionInterpolator& playbackClock;
      std::optional<std::chrono::milliseconds>& optPreviewElapsed;
      CoverArtLoader& coverArt;
      bool kittyCoverArt = false;
      uimodel::AobusSoulAnimationState soulAnimation{};
      std::optional<uimodel::FrameClock::TimePoint> optPreviousSoulFrameTime;

      ftxui::Element operator()()
      {
        using namespace ftxui;

        auto const frameBuildScope = frameTimer.measureBuild();
        auto const selectedTrackView = library.selectedTrackView();
        auto const selectedCoverArtId = selectedTrackView.coverArtId;
        auto const detailVisible = shell.overlay() == Overlay::DetailPanel;

        if (detailVisible)
        {
          coverArt.request(selectedCoverArtId);
        }

        if (!detailVisible)
        {
          hitRegions.coverBox = ftxui::Box{};
        }

        auto coverElementPtr =
          kittyCoverArt ? renderKittyCoverArtPlaceholder(textCatalog, coverArt.kittyPng() != std::nullopt) |
                            reflect(hitRegions.coverBox)
                        : renderCoverArtPreview(textCatalog, coverArt.preview()) | reflect(hitRegions.coverBox);
        auto const currentListTitle = library.currentListTitle();
        auto const& state = playback.snapshot().transport;
        hitRegions.clearFrameLocalRows();
        auto const frameTime = monotonicFrameTime();
        auto const soulMotionMode = uimodel::aobusSoulMotionMode(state.transport);
        soulAnimation.setMotionMode(soulMotionMode);

        if (soulMotionMode == uimodel::AobusSoulMotionMode::Animating)
        {
          if (optPreviousSoulFrameTime)
          {
            soulAnimation.advance(frameTime - *optPreviousSoulFrameTime);
          }

          optPreviousSoulFrameTime = frameTime;
        }
        else
        {
          optPreviousSoulFrameTime.reset();
        }

        auto const displayElapsed = optPreviewElapsed.value_or(playbackClock.interpolateElapsed(frameTime));
        auto const animationElapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(frameTime.time_since_epoch());
        auto const& presentation = runtime.views().trackListPresentation(library.activeViewId());
        auto const terminalSize = ftxui::Terminal::Size();
        auto const terminalColumns = terminalSize.dimx;
        auto const terminalRows = terminalSize.dimy;
        auto const playbackRows = playbackBarRows(terminalRows);
        auto const hoveredButton = shell.isCommandActive() ? HoveredButton::None : events.hoveredButton();
        auto tableElementPtr =
          trackTableView(textCatalog,
                         tuiTextCatalog,
                         library.tracks(),
                         library.sections(),
                         library.selectedTrack(),
                         state.nowPlaying.trackId,
                         presentation,
                         TrackTableViewOptions{.columnWidths = &trackColumnWidthOverrides,
                                               .resizeHandles = &hitRegions.trackColumnResizeHandles,
                                               .sectionRowHitRegions = &hitRegions.trackSectionRows,
                                               .tableBox = &hitRegions.trackTableBox,
                                               .availableColumns = std::max(1, terminalColumns - 2),
                                               .viewportRows = terminalRows});
        auto const presentationTitle = trackPresentationDisplayId(textCatalog, presentation.id);
        auto workspaceElementPtr =
          style::titledPanel(
            "",
            std::move(tableElementPtr),
            style::PanelOptions{
              .leftFooter = style::PanelEdgeButton{.label = tuiTextCatalog.text(TuiTextId::WorkspaceList),
                                                   .value = currentListTitle,
                                                   .box = &hitRegions.libraryButtonBox,
                                                   .hovered = hoveredButton == HoveredButton::Library},
              .leftFooterRight = style::PanelEdgeButton{.label = tuiTextCatalog.text(TuiTextId::WorkspaceView),
                                                        .value = presentationTitle,
                                                        .box = &hitRegions.presentationButtonBox,
                                                        .hovered = hoveredButton == HoveredButton::Presentation},
              .rightFooter = selectionSummary(textCatalog, library.tracks().size(), library.selectedTrack())}) |
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
            auto const panelColumns =
              libraryChooserPaneColumns(tuiTextCatalog, library.libraryLabels(), terminalColumns);
            auto const panelRows =
              static_cast<std::int32_t>(std::max<std::size_t>(1, library.libraryLabels().size())) + 4;
            popoverElementPtr = mainLayerPopover(
              hitRegions.libraryButtonBox,
              AnchoredOverlayPlacement::Above,
              panelColumns,
              panelRows,
              libraryChooserPane(tuiTextCatalog, library.libraryLabels(), library.selectedList(), panelColumns));
            break;
          }
          case Overlay::DetailPanel:
          {
            auto const panelColumns =
              detailPaneColumns(textCatalog, selectedTrackView.track, sidePanelColumnsLimit(terminalColumns));
            mainContentPtr = hbox({
              workspaceElementPtr,
              detailPane(textCatalog, selectedTrackView.track, std::move(coverElementPtr), panelColumns),
            });
            break;
          }
          case Overlay::QualityPanel:
          {
            auto const panelColumns = qualityPanelColumns(tuiTextCatalog, textCatalog, state, terminalColumns);
            popoverElementPtr = mainLayerPopover(hitRegions.soulButtonBox,
                                                 AnchoredOverlayPlacement::Below,
                                                 panelColumns,
                                                 0,
                                                 qualityPanel(tuiTextCatalog, textCatalog, state, panelColumns));
            break;
          }
          case Overlay::OutputDevices:
          {
            auto const panelColumns =
              outputDevicePanelColumns(tuiTextCatalog, outputDevices.viewState(), terminalColumns);
            popoverElementPtr = mainLayerPopover(hitRegions.outputDeviceButtonBox,
                                                 AnchoredOverlayPlacement::Below,
                                                 panelColumns,
                                                 0,
                                                 outputDevicePanel(tuiTextCatalog,
                                                                   outputDevices.viewState(),
                                                                   outputDevices.selectedRow(),
                                                                   &hitRegions.outputDeviceRows,
                                                                   panelColumns));
            break;
          }
          case Overlay::PresentationPanel:
          case Overlay::Notifications: break;
          case Overlay::Help:
          {
            auto const panelColumns = helpPaneColumns(tuiTextCatalog, sidePanelColumnsLimit(terminalColumns));
            mainContentPtr = hbox({
              workspaceElementPtr,
              helpPane(tuiTextCatalog, panelColumns),
            });
            break;
          }
        }

        if (!shell.isCommandActive() && shell.overlay() == Overlay::None && popoverElementPtr == nullptr &&
            events.isQualityHoverVisible())
        {
          auto const panelColumns = qualityPanelColumns(tuiTextCatalog, textCatalog, state, terminalColumns);
          popoverElementPtr = mainLayerPopover(hitRegions.soulButtonBox,
                                               AnchoredOverlayPlacement::Below,
                                               panelColumns,
                                               0,
                                               qualityPanel(tuiTextCatalog, textCatalog, state, panelColumns));
        }

        auto mainLayerPtr = popoverElementPtr == nullptr ? std::move(mainContentPtr)
                                                         : dbox({
                                                             std::move(mainContentPtr),
                                                             std::move(popoverElementPtr),
                                                           });
        auto rootPtr = vbox({
          playbackBar(tuiTextCatalog,
                      PlaybackBarViewState{.playbackState = &state,
                                           .displayElapsed = displayElapsed,
                                           .animationElapsed = animationElapsed,
                                           .soulMotion = soulAnimation.motionFrame(),
                                           .outputView = &outputDevices.viewState(),
                                           .outputDeviceBox = &hitRegions.outputDeviceButtonBox,
                                           .soulButtonBox = &hitRegions.soulButtonBox,
                                           .seekRailBox = &hitRegions.seekRailBox,
                                           .outputDeviceHovered = hoveredButton == HoveredButton::OutputDevice,
                                           .terminalColumns = terminalColumns}),
          std::move(mainLayerPtr) | flex,
          statusBar(tuiTextCatalog,
                    StatusBarViewState{.activityStatus = &activityStatusViewModel.viewState(),
                                       .terminalColumns = terminalColumns,
                                       .filterDraft = library.filterDraft(),
                                       .shell = &shell,
                                       .activityStatusBox = &hitRegions.activityStatusBox,
                                       .activityStatusHovered = hoveredButton == HoveredButton::ActivityStatus}),
        });

        if (auto commandPopoverPtr =
              commandPalettePopover(textCatalog, tuiTextCatalog, shell, terminalColumns, terminalRows);
            commandPopoverPtr != nullptr)
        {
          return dbox({
            std::move(rootPtr),
            std::move(commandPopoverPtr),
          });
        }

        if (auto presentationPopoverPtr = presentationPopover(textCatalog,
                                                              tuiTextCatalog,
                                                              shell,
                                                              library,
                                                              hitRegions.presentationButtonBox,
                                                              terminalColumns,
                                                              &hitRegions.presentationRows);
            presentationPopoverPtr != nullptr)
        {
          return dbox({
            std::move(rootPtr),
            std::move(presentationPopoverPtr),
          });
        }

        if (auto notificationPopoverPtr = notificationPopover(textCatalog,
                                                              tuiTextCatalog,
                                                              shell,
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
      }
    };

    /**
     * @brief Opens the TUI's own application-preference file.
     *
     * The groups and schema are shared with every other frontend, but the file
     * is not: `ConfigStore` writes a whole document from the snapshot it took at
     * first read, so two frontends pointed at one file would drop each other's
     * groups whenever they ran at the same time.
     *
     * When the platform names no location, or the directory cannot be made, the
     * store is one that keeps nothing rather than no store at all: the session
     * is degraded, not failed, and every caller below is spared a null check for
     * a case none of them can do anything about.
     */
    std::unique_ptr<rt::ConfigStore> openAppConfigStore()
    {
      auto dirRes = utility::applicationConfigDirectory();

      if (!dirRes)
      {
        APP_LOG_INFO("TUI: keeping no application preferences: {}", dirRes.error().message);
        return std::make_unique<rt::ConfigStore>(rt::ConfigStore::NoLocation{});
      }

      auto const path = *dirRes / "tui.yaml";
      auto ec = std::error_code{};
      std::filesystem::create_directories(path.parent_path(), ec);

      if (ec)
      {
        APP_LOG_WARN("TUI: keeping no application preferences: {}", ec.message());
        return std::make_unique<rt::ConfigStore>(rt::ConfigStore::NoLocation{});
      }

      return std::make_unique<rt::ConfigStore>(path);
    }

    /**
     * @brief Where derived caches go, or nothing.
     *
     * The runtime does not discover platform application directories, so this
     * shell resolves the location and hands it over. Nothing here is
     * authoritative: when the platform names no location the session runs without
     * a cache, and cover reads re-extract from the media files instead.
     */
    std::filesystem::path resolveCacheDirectory()
    {
      auto dirRes = utility::applicationCacheDirectory();

      if (!dirRes)
      {
        APP_LOG_INFO("TUI: caching no cover art: {}", dirRes.error().message);
        return {};
      }

      return *std::move(dirRes);
    }

    /// Records the exact route a user picked, so the next session can ask for it again.
    uimodel::OutputDeviceIntent makeOutputDeviceIntent(rt::ConfigStore& store)
    {
      return uimodel::OutputDeviceIntent::recordedBy(
        [configStore = &store](audio::OutputDeviceSelection const& selection)
        {
          auto prefs = rt::AppPrefsState{};
          rt::loadAppPrefs(*configStore, prefs);
          prefs.preferredOutputSelection = selection;

          if (auto const result = rt::saveAppPrefs(*configStore, prefs); !result)
          {
            APP_LOG_WARN("TUI: failed to record the requested output route: {}", result.error().message);
          }
        });
    }

    /// Resubmits a persisted route once platform providers have published their catalog.
    void restoreOutputDeviceSelection(rt::ConfigStore& store, rt::PlaybackService& playback)
    {
      auto prefs = rt::AppPrefsState{};
      rt::loadAppPrefs(store, prefs);

      // The last-active route is the desktop shells' fallback, saved when their
      // window closes. This shell keeps no session document, so there is
      // nothing to fall back to and the explicit preference decides alone.
      auto const optSelection = uimodel::resolveOutputDeviceSelectionToRestore(
        prefs.preferredOutputSelection, {}, playback.snapshot().transport.output);

      if (optSelection)
      {
        playback.commands().setOutputDevice(optSelection->backendId, optSelection->deviceId, optSelection->profileId);
      }
    }
  } // namespace

  std::int32_t run(AppOptions const& options,
                   uimodel::PresentationTextCatalog const& textCatalog,
                   TuiTextCatalog const& tuiTextCatalog,
                   rt::TextOrderingPolicy const& textOrderingPolicy)
  {
    auto const coverArtMode = parseCoverArtMode(options.coverArtMode);
    auto const kittyCoverArt = shouldUseKittyCoverArt(coverArtMode);
    auto const blockCoverArt = shouldUseBlockCoverArt(coverArtMode);
    auto coverArtDeliveryMode = CoverArtDeliveryMode::Off;

    if (kittyCoverArt)
    {
      coverArtDeliveryMode = CoverArtDeliveryMode::Kitty;
    }
    else if (blockCoverArt)
    {
      coverArtDeliveryMode = CoverArtDeliveryMode::Blocks;
    }

    std::filesystem::create_directories(options.configPath.parent_path());
    // Logging comes up first: opening the preference store is the first thing
    // that can degrade, and its one explanation of why this session keeps
    // nothing would otherwise be written to the null sink that stands in until
    // initialize() runs.
    rt::Log::initialize(
      options.logLevel, rt::LibraryPaths{options.libraryRoot}.logsPath(), rt::LogConsoleMode::Disabled);
    auto const logShutdown = gsl_lite::finally([] { rt::Log::shutdown(); });
    auto const appConfigStorePtr = openAppConfigStore();
    auto screen = ftxui::ScreenInteractive::FullscreenAlternateScreen();
    screen.TrackMouse(true);
    auto executorPtr = std::make_unique<Executor>(screen);
    auto* const executor = executorPtr.get();
    auto runtimeRes = rt::AppRuntime::create(rt::AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = options.libraryRoot,
      .databasePath = options.databasePath,
      .cacheDirectory = resolveCacheDirectory(),
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(options.configPath),
      .textOrderingPolicy = &textOrderingPolicy,
    });

    if (!runtimeRes)
    {
      std::println(stderr, "Failed to open library: {}", runtimeRes.error().message);
      return 1;
    }

    auto runtimePtr = std::move(*runtimeRes);
    auto& runtime = *runtimePtr;

    registerPlatformAudioBackends(runtime);
    restoreOutputDeviceSelection(*appConfigStorePtr, runtime.playback());

    auto library = LibraryController{runtime, textCatalog, tuiTextCatalog};
    auto shell = ShellInteractionModel{};
    auto hitRegions = TuiHitRegions{};
    auto trackColumnWidthOverrides = std::vector<TrackColumnWidthOverride>{};
    auto kittyPaintState = KittyPaintState{};

    auto& playback = runtime.playback();
    auto requestRefresh = [&screen] { screen.PostEvent(ftxui::Event::Custom); };
    auto resourceByteLoader = rt::ResourceByteLoader{runtime};
    auto coverArt = CoverArtLoader{resourceByteLoader, runtime.async(), coverArtDeliveryMode, requestRefresh};
    auto clockTickActive = std::atomic_bool{shouldTickTransportClock(playback.snapshot().transport.transport)};
    auto activityAutoDismissActive = std::atomic_bool{false};
    auto playbackClock = uimodel::PlaybackPositionInterpolator{};
    auto optPreviewElapsed = std::optional<std::chrono::milliseconds>{};
    auto playbackTime = uimodel::PlaybackPositionViewModel{
      playback,
      [&](uimodel::PlaybackPositionViewState const& view)
      {
        clockTickActive.store(shouldTickTransportClock(playback.snapshot().transport.transport));

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
      textCatalog,
      [&](uimodel::ActivityStatusViewState const& view)
      {
        activityAutoDismissActive.store(view.compact.optAutoDismissTimeout.has_value());
        requestRefresh();
      },
      uimodel::ActivityStatusViewModelOptions{.libraryTasks = &runtime.library().taskService()}};
    runtime.notifications().post(rt::NotificationSeverity::Info,
                                 std::string{tuiTextCatalog.text(TuiTextId::LibraryReady)},
                                 rt::NotificationLifetime::transient());

    auto playbackSub =
      playback.events().onSnapshot([requestRefresh](rt::PlaybackSnapshot const&) { requestRefresh(); });
    auto outputDevices =
      OutputDeviceController{playback, textCatalog, makeOutputDeviceIntent(*appConfigStorePtr), requestRefresh};
    auto commandCompletions =
      CommandCompletionProvider{runtime.completion(), runtime.workspace(), textCatalog, tuiTextCatalog};
    auto events = EventController{screen,
                                  shell,
                                  library,
                                  runtime,
                                  EventControllerBindings{
                                    .outputDevices = &outputDevices,
                                    .hitRegions = &hitRegions,
                                    .trackColumnWidthOverrides = &trackColumnWidthOverrides,
                                    .activityStatusViewModel = &activityStatusViewModel,
                                    .notifications = &runtime.notifications(),
                                    .commandCompletionCallback = [&commandCompletions](std::string_view const draft)
                                    { return commandCompletions.complete(draft); },
                                  }};

    auto frameTimer = FrameTimer{};
    auto frameRenderer = AppFrameRenderer{
      .frameTimer = frameTimer,
      .textCatalog = textCatalog,
      .tuiTextCatalog = tuiTextCatalog,
      .library = library,
      .shell = shell,
      .runtime = runtime,
      .playback = playback,
      .outputDevices = outputDevices,
      .activityStatusViewModel = activityStatusViewModel,
      .events = events,
      .hitRegions = hitRegions,
      .trackColumnWidthOverrides = trackColumnWidthOverrides,
      .playbackClock = playbackClock,
      .optPreviewElapsed = optPreviewElapsed,
      .coverArt = coverArt,
      .kittyCoverArt = kittyCoverArt,
      .soulAnimation = {},
      .optPreviousSoulFrameTime = std::nullopt,
    };
    auto rendererPtr = ftxui::Renderer([&frameRenderer] { return frameRenderer(); });

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
      frameTimer.recordPresentIfDrawn();

      activityStatusViewModel.autoDismissCompactIfDue();

      if (kittyCoverArt)
      {
        updateKittyCoverArt(kittyPaintState, shell, coverArt.resourceId(), hitRegions.coverBox, coverArt.kittyPng());
      }
    }

    if (kittyCoverArt && kittyPaintState.visible)
    {
      std::print("{}", kittyDeleteImageEscape(kKittyCoverArtImageId));
      std::fflush(stdout);
    }

    coverArt.cancel();
    playback.commands().stop();
    runtime.shutdown();
    frameTimer.flush();
    return 0;
  }
} // namespace ao::tui
