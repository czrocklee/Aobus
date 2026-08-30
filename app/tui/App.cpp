// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "App.h"

#include "AnchoredOverlay.h"
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
#include "TerminalTrackColumnLayout.h"
#include "TrackPresentationNavigation.h"
#include "TrackTable.h"
#include "TuiHitRegions.h"
#include "TuiKeymap.h"
#include "TuiLayoutStateStore.h"
#include "TuiText.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/AppState.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/input/KeymapStore.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputSelection.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
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

    ftxui::Element commandPalettePopover(i18n::MessageCatalog const& textCatalog,
                                         ShellInteractionModel const& shell,
                                         TuiKeymapPlan const& keymapPlan,
                                         std::int32_t const terminalColumns,
                                         std::int32_t const terminalRows)
    {
      if (shell.inputMode() != ShellInputMode::Command)
      {
        return {};
      }

      auto const panelColumns = commandPalettePanelColumns(terminalColumns);
      auto const panelRows = commandPalettePanelRows(terminalRows);

      return centerPopover(commandPalettePanel(textCatalog, shell, keymapPlan, panelColumns) |
                           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, panelColumns) |
                           ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, panelRows));
    }

    ftxui::Element quickFilterPopover(i18n::MessageCatalog const& textCatalog,
                                      ShellInteractionModel const& shell,
                                      TuiKeymapPlan const& keymapPlan,
                                      std::string_view const filterError,
                                      std::int32_t const terminalColumns,
                                      std::int32_t const terminalRows)
    {
      if (shell.inputMode() != ShellInputMode::QuickFilter)
      {
        return {};
      }

      auto const panelColumns = commandPalettePanelColumns(terminalColumns);
      auto const panelRows = quickFilterPanelRows(shell, !filterError.empty(), terminalRows);

      return anchoredOverlay(quickFilterCompletionPanel(textCatalog, shell, keymapPlan, panelColumns, filterError) |
                               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, panelColumns) |
                               ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, panelRows),
                             {},
                             AnchoredOverlayPlacement::Above,
                             AnchoredOverlaySize{.columns = panelColumns, .rows = panelRows},
                             AnchoredOverlayTerminal{.columns = terminalColumns, .rows = terminalRows},
                             AnchoredOverlayOptions{.fallbackToBottom = true});
    }

    ftxui::Element presentationPopover(i18n::MessageCatalog const& textCatalog,
                                       ShellInteractionModel const& shell,
                                       TuiKeymapPlan const& keymapPlan,
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
        textCatalog, library.presentationEntries(), activePresentationId, keymapPlan, terminalColumns);

      return anchoredOverlay(presentationPanel(textCatalog,
                                               library.presentationEntries(),
                                               activePresentationId,
                                               library.selectedPresentation(),
                                               keymapPlan,
                                               rowHitRegions,
                                               panelColumns) |
                               ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, kPresentationPanelRows),
                             presentationButtonBox,
                             AnchoredOverlayPlacement::Above,
                             AnchoredOverlaySize{.columns = panelColumns, .rows = kPresentationPanelRows},
                             AnchoredOverlayTerminal{.columns = terminalColumns});
    }

    ftxui::Element notificationPopover(i18n::MessageCatalog const& textCatalog,
                                       ShellInteractionModel const& shell,
                                       TuiKeymapPlan const& keymapPlan,
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

      auto const panelColumns = notificationCenterPanelColumns(textCatalog, state, keymapPlan, terminalColumns);

      return anchoredOverlay(notificationCenterPanel(textCatalog, state, keymapPlan, rowHitRegions, panelColumns) |
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

    struct AppFrameRenderer final
    {
      FrameTimer& frameTimer;
      i18n::MessageCatalog const& textCatalog;
      LibraryController& library;
      ShellInteractionModel& shell;
      TuiKeymapPlan const& keymapPlan;
      rt::PlaybackService& playback;
      OutputDeviceController& outputDevices;
      uimodel::ActivityStatusViewModel& activityStatusViewModel;
      EventController& events;
      TuiHitRegions& hitRegions;
      uimodel::TrackColumnLayouts& trackColumnLayouts;
      TrackColumnResizePreview& trackColumnResizePreview;
      uimodel::PlaybackPositionInterpolator& playbackClock;
      std::optional<std::chrono::milliseconds>& optPreviewElapsed;
      CoverArtLoader& coverArt;
      CoverArtDeliveryMode coverArtMode = CoverArtDeliveryMode::Off;
      std::int32_t coverColumns = kCoverArtDefaultColumns;
      uimodel::AobusSoulAnimationState soulAnimation{};
      std::optional<uimodel::FrameClock::TimePoint> optPreviousSoulFrameTime;

      ftxui::Element operator()()
      {
        using namespace ftxui;

        auto const frameBuildScope = frameTimer.measureBuild();
        auto const selectedTrackView = library.selectedTrackView();
        auto const terminalSize = ftxui::Terminal::Size();
        auto const terminalColumns = terminalSize.dimx;
        auto const terminalRows = terminalSize.dimy;
        auto const playbackRows = playbackBarRows(terminalRows);
        auto const mainContentRows = terminalRows - playbackRows - kStatusBarRows;
        auto const detailVisible = shell.overlay() == Overlay::DetailPanel;
        auto const coverArtVisible =
          detailVisible && selectedTrackView.track != nullptr && detailPaneShowsCoverArt(mainContentRows);

        // Artwork nobody can see is still a resource read and a transform, so
        // the request follows what the frame will actually show.
        if (coverArtVisible)
        {
          coverArt.request(selectedTrackView.coverArtId);
        }
        else
        {
          coverArt.clear();
        }

        auto coverElementPtr = detailCoverArt(textCatalog,
                                              coverArtVisible ? coverArtMode : CoverArtDeliveryMode::Off,
                                              coverArt.preview(),
                                              coverArt.kittyPng(),
                                              coverColumns,
                                              &hitRegions.coverBox);
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
        auto const& presentation = library.activePresentation();
        auto const sidePanelLimit = sidePanelColumnsLimit(terminalColumns);
        auto const detailPanelColumns =
          detailVisible ? detailPaneColumns(textCatalog, sidePanelLimit, coverColumns) : 0;
        auto const helpPanelColumns =
          shell.overlay() == Overlay::Help ? helpPaneColumns(textCatalog, keymapPlan, sidePanelLimit) : 0;
        auto const sidePaneColumns = std::max(detailPanelColumns, helpPanelColumns);
        auto const availableTrackColumns = std::max(1, terminalColumns - sidePaneColumns - 2);
        auto const listId = library.currentListId();
        auto const& storedLayout = trackColumnResizePreview.listId == listId ? trackColumnResizePreview.layout
                                                                             : trackColumnLayouts.layoutForList(listId);
        auto const terminalColumnLayout =
          projectTerminalTrackColumnLayout(presentation, storedLayout, availableTrackColumns);
        auto const hoveredButton = shell.isInputActive() ? HoveredButton::None : events.hoveredButton();
        auto tableElementPtr =
          trackTableView(textCatalog,
                         library.tracks(),
                         library.sections(),
                         library.selectedTrack(),
                         state.nowPlaying.trackId,
                         presentation,
                         TrackTableViewOptions{.columnLayout = &terminalColumnLayout,
                                               .resizeHandles = &hitRegions.trackColumnResizeHandles,
                                               .sectionRowHitRegions = &hitRegions.trackSectionRows,
                                               .tableBox = &hitRegions.trackTableBox,
                                               .availableColumns = availableTrackColumns,
                                               .viewportRows = terminalRows});
        auto const presentationTitle = trackPresentationDisplayId(textCatalog, presentation.id);
        auto workspaceElementPtr =
          style::titledPanel(
            "",
            std::move(tableElementPtr),
            style::PanelOptions{
              .leftFooter =
                style::PanelEdgeButton{.label = tuiChromeText(textCatalog, i18n::MessageId::TuiShellWorkspaceList),
                                       .value = currentListTitle,
                                       .box = &hitRegions.libraryButtonBox,
                                       .hovered = hoveredButton == HoveredButton::Library},
              .leftFooterRight =
                style::PanelEdgeButton{.label = tuiChromeText(textCatalog, i18n::MessageId::TuiShellWorkspaceView),
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
              libraryChooserPaneColumns(textCatalog, library.libraryLabels(), keymapPlan, terminalColumns);
            auto const panelRows =
              static_cast<std::int32_t>(std::max<std::size_t>(1, library.libraryLabels().size())) + 4;
            popoverElementPtr = mainLayerPopover(
              hitRegions.libraryButtonBox,
              AnchoredOverlayPlacement::Above,
              panelColumns,
              panelRows,
              libraryChooserPane(
                textCatalog, library.libraryLabels(), library.selectedList(), keymapPlan, panelColumns));
            break;
          }
          case Overlay::DetailPanel:
          {
            mainContentPtr = hbox({
              workspaceElementPtr,
              detailPane(textCatalog, selectedTrackView.track, std::move(coverElementPtr), detailPanelColumns),
            });
            break;
          }
          case Overlay::QualityPanel:
          {
            auto const panelColumns = qualityPanelColumns(textCatalog, state, keymapPlan, terminalColumns);
            popoverElementPtr = mainLayerPopover(hitRegions.soulButtonBox,
                                                 AnchoredOverlayPlacement::Below,
                                                 panelColumns,
                                                 0,
                                                 qualityPanel(textCatalog, state, keymapPlan, panelColumns));
            break;
          }
          case Overlay::OutputDevices:
          {
            auto const panelColumns =
              outputDevicePanelColumns(textCatalog, outputDevices.viewState(), keymapPlan, terminalColumns);
            popoverElementPtr = mainLayerPopover(hitRegions.outputDeviceButtonBox,
                                                 AnchoredOverlayPlacement::Below,
                                                 panelColumns,
                                                 0,
                                                 outputDevicePanel(textCatalog,
                                                                   outputDevices.viewState(),
                                                                   outputDevices.selectedRow(),
                                                                   keymapPlan,
                                                                   &hitRegions.outputDeviceRows,
                                                                   panelColumns));
            break;
          }
          case Overlay::PresentationPanel:
          case Overlay::Notifications: break;
          case Overlay::Help:
          {
            mainContentPtr = hbox({
              workspaceElementPtr,
              helpPane(textCatalog, keymapPlan, helpPanelColumns),
            });
            break;
          }
        }

        if (!shell.isInputActive() && shell.overlay() == Overlay::None && popoverElementPtr == nullptr &&
            events.isQualityHoverVisible())
        {
          auto const panelColumns = qualityPanelColumns(textCatalog, state, keymapPlan, terminalColumns);
          popoverElementPtr = mainLayerPopover(hitRegions.soulButtonBox,
                                               AnchoredOverlayPlacement::Below,
                                               panelColumns,
                                               0,
                                               qualityPanel(textCatalog, state, keymapPlan, panelColumns));
        }

        auto mainLayerPtr = popoverElementPtr == nullptr ? std::move(mainContentPtr)
                                                         : dbox({
                                                             std::move(mainContentPtr),
                                                             std::move(popoverElementPtr),
                                                           });
        auto rootPtr = vbox({
          playbackBar(textCatalog,
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
          statusBar(textCatalog,
                    StatusBarViewState{.activityStatus = &activityStatusViewModel.viewState(),
                                       .terminalColumns = terminalColumns,
                                       .filterDraft = library.filterDraft(),
                                       .shell = &shell,
                                       .activityStatusBox = &hitRegions.activityStatusBox,
                                       .activityStatusHovered = hoveredButton == HoveredButton::ActivityStatus},
                    keymapPlan),
        });

        auto visibleFilterError = std::string_view{};

        if (shell.inputMode() == ShellInputMode::QuickFilter && shell.isInputTouched() &&
            shell.inputDraft() == library.filterDraft())
        {
          visibleFilterError = library.filterError();
        }

        if (auto commandPopoverPtr =
              commandPalettePopover(textCatalog, shell, keymapPlan, terminalColumns, terminalRows);
            commandPopoverPtr != nullptr)
        {
          return dbox({
            std::move(rootPtr),
            std::move(commandPopoverPtr),
          });
        }

        if (auto quickFilterPopoverPtr =
              quickFilterPopover(textCatalog, shell, keymapPlan, visibleFilterError, terminalColumns, terminalRows);
            quickFilterPopoverPtr != nullptr)
        {
          return dbox({
            std::move(rootPtr),
            std::move(quickFilterPopoverPtr),
          });
        }

        if (auto presentationPopoverPtr = presentationPopover(textCatalog,
                                                              shell,
                                                              keymapPlan,
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
                                                              shell,
                                                              keymapPlan,
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
    std::optional<std::filesystem::path> resolveAppConfigPath()
    {
      auto dirRes = utility::applicationConfigDirectory();

      if (!dirRes)
      {
        APP_LOG_INFO("TUI: keeping no application preferences: {}", dirRes.error().message);
        return std::nullopt;
      }

      return *dirRes / "tui.yaml";
    }

    std::unique_ptr<rt::ConfigStore> openAppConfigStore(std::optional<std::filesystem::path> const& optAppConfigPath)
    {
      if (!optAppConfigPath)
      {
        return std::make_unique<rt::ConfigStore>(rt::ConfigStore::NoLocation{});
      }

      auto ec = std::error_code{};
      std::filesystem::create_directories(optAppConfigPath->parent_path(), ec);

      if (ec)
      {
        APP_LOG_WARN("TUI: keeping no application preferences: {}", ec.message());
        return std::make_unique<rt::ConfigStore>(rt::ConfigStore::NoLocation{});
      }

      return std::make_unique<rt::ConfigStore>(*optAppConfigPath);
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
      // window closes. The TUI runtime-session document deliberately excludes
      // output identity, so its explicit global preference decides alone.
      auto const optSelection = uimodel::resolveOutputDeviceSelectionToRestore(
        prefs.preferredOutputSelection, {}, playback.snapshot().transport.output);

      if (optSelection)
      {
        playback.commands().setOutputDevice(optSelection->backendId, optSelection->deviceId, optSelection->profileId);
      }
    }
  } // namespace

  std::int32_t run(AppOptions const& options,
                   i18n::MessageCatalog const& textCatalog,
                   rt::TextOrderingPolicy const& textOrderingPolicy,
                   rt::CompletionAliasPolicy const& completionAliasPolicy)
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

    auto workspaceConfigDirectoryEc = std::error_code{};
    std::filesystem::create_directories(options.configPath.parent_path(), workspaceConfigDirectoryEc);

    if (workspaceConfigDirectoryEc)
    {
      std::println(stderr,
                   "Failed to prepare the TUI workspace configuration directory: {}",
                   workspaceConfigDirectoryEc.message());
      return 1;
    }

    // Logging comes up before opening the optional preference store: its one
    // explanation of why this session keeps nothing would otherwise be written
    // to the null sink that stands in until initialize() runs.
    rt::Log::initialize(
      options.logLevel, rt::LibraryPaths{options.libraryRoot}.logsPath(), rt::LogConsoleMode::Disabled);
    auto const logShutdown = gsl_lite::finally([] { rt::Log::shutdown(); });
    auto const optAppConfigPath = resolveAppConfigPath();

    if (auto const validatedRes =
          validateTuiConfigStorePaths(options.libraryRoot, options.configPath, optAppConfigPath);
        !validatedRes)
    {
      std::println(stderr, "Invalid TUI managed-state paths: {}", validatedRes.error().message);
      return 1;
    }

    auto const appConfigStorePtr = openAppConfigStore(optAppConfigPath);
    auto const keymap = uimodel::loadKeymap(*appConfigStorePtr, tuiDefaultKeymap());
    auto const keymapPlan = TuiKeymapPlan{keymap};
    // Declared before AppRuntime so the executor's borrowed screen reference
    // remains valid through runtime shutdown and destruction.
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
      .completionAliasPolicy = &completionAliasPolicy,
    });

    if (!runtimeRes)
    {
      std::println(stderr, "Failed to open library: {}", runtimeRes.error().message);
      return 1;
    }

    auto runtimePtr = std::move(*runtimeRes);
    auto& runtime = *runtimePtr;

    for (auto& providerPtr : audio::createPlatformBackendProviders())
    {
      runtime.addAudioProvider(std::move(providerPtr));
    }

    restoreOutputDeviceSelection(*appConfigStorePtr, runtime.playback());

    if (auto const restoredRes = runtime.workspace().restoreSession(runtime.workspaceConfigStore()); !restoredRes)
    {
      APP_LOG_WARN("TUI: failed to restore the workspace session: {}", restoredRes.error().message);
    }

    // Declared before every frontend observer so reverse destruction retires
    // all callback targets before Runtime stops producers and its executor.
    auto const runtimeShutdown = gsl_lite::finally([&runtime] { runtime.shutdown(); });
    auto requestRefresh = [&screen] { screen.PostEvent(ftxui::Event::Custom); };
    auto layoutStateStore = TuiLayoutStateStore{options.libraryRoot};
    auto restoredColumnLayouts = uimodel::TrackColumnLayouts::Snapshot{};
    auto restoredListPresentations = uimodel::ListPresentations::Snapshot{};
    layoutStateStore.load(restoredColumnLayouts, restoredListPresentations);

    auto presentationCatalog = uimodel::TrackPresentationCatalog{runtime.workspace(), textCatalog};
    auto listPresentations = uimodel::ListPresentations{presentationCatalog, runtime.library().changes()};
    listPresentations.restore(std::move(restoredListPresentations));
    auto trackColumnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    trackColumnLayouts.restore(std::move(restoredColumnLayouts));
    // Input dispatch and Runtime library callbacks share the screen executor,
    // keeping this one unsynchronized ConfigStore writer serial. A loop-turn
    // checkpoint folds every per-list signal in one LibraryChangeSet into the
    // final combined document instead of writing intermediate model states.
    bool layoutStateDirty = false;
    bool layoutCheckpointRequested = false;
    auto saveLayoutState = [&]
    {
      if (auto const savedRes = layoutStateStore.save(trackColumnLayouts.snapshot(), listPresentations.snapshot());
          !savedRes)
      {
        APP_LOG_WARN("TUI: failed to persist layout state: {}", savedRes.error().message);
        return;
      }

      layoutStateDirty = false;
    };
    auto requestLayoutCheckpoint = [&]
    {
      layoutStateDirty = true;

      if (!std::exchange(layoutCheckpointRequested, true))
      {
        requestRefresh();
      }
    };
    auto columnLayoutsSub =
      trackColumnLayouts.signalChanged().connect([&](ListId const) { requestLayoutCheckpoint(); });
    auto listPresentationsSub =
      listPresentations.signalChanged().connect([&](ListId const) { requestLayoutCheckpoint(); });
    auto library = LibraryController{runtime, textCatalog, listPresentations};
    runtime.startPlaybackSessionPersistence();

    if (auto const restoredRes = runtime.restorePlaybackSession(); !restoredRes)
    {
      APP_LOG_WARN("TUI: failed to restore the playback session: {}", restoredRes.error().message);
    }

    auto shell = ShellInteractionModel{};
    auto hitRegions = TuiHitRegions{};
    auto trackColumnResizePreview = TrackColumnResizePreview{};
    auto kittyPaintState = KittyPaintState{};
    auto const cellAspectRatio = queryTerminalCellAspectRatio();
    auto const coverColumns = coverArtColumns(kCoverArtRows, cellAspectRatio);

    auto& playback = runtime.playback();
    auto coverArt =
      CoverArtLoader{runtime.resourceBytes(), runtime.async(), coverArtDeliveryMode, requestRefresh, coverColumns};
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
      uimodel::ActivityStatusViewModelOptions{.libraryJobs = &runtime.library().jobs()}};
    runtime.notifications().post(rt::NotificationSeverity::Info,
                                 tuiChromeText(textCatalog, i18n::MessageId::TuiLibraryReady),
                                 rt::NotificationLifetime::transient());

    auto playbackSub =
      playback.events().onSnapshot([requestRefresh](rt::PlaybackSnapshot const&) { requestRefresh(); });
    auto outputDevices =
      OutputDeviceController{playback, textCatalog, makeOutputDeviceIntent(*appConfigStorePtr), requestRefresh};
    auto commandCompletions = CommandCompletionProvider{runtime.completion(), runtime.workspace(), textCatalog};
    auto events = EventController{screen,
                                  shell,
                                  library,
                                  runtime,
                                  keymapPlan,
                                  EventControllerBindings{
                                    .outputDevices = &outputDevices,
                                    .hitRegions = &hitRegions,
                                    .trackColumnLayouts = &trackColumnLayouts,
                                    .trackColumnResizePreview = &trackColumnResizePreview,
                                    .activityStatusViewModel = &activityStatusViewModel,
                                    .notifications = &runtime.notifications(),
                                    .commandCompletionCallback = [&commandCompletions](std::string_view const draft)
                                    { return commandCompletions.completeCommand(draft); },
                                    .filterCompletionCallback = [&commandCompletions](std::string_view const draft)
                                    { return commandCompletions.completeFilter(draft); },
                                  }};

    auto frameTimer = FrameTimer{};
    auto frameRenderer = AppFrameRenderer{
      .frameTimer = frameTimer,
      .textCatalog = textCatalog,
      .library = library,
      .shell = shell,
      .keymapPlan = keymapPlan,
      .playback = playback,
      .outputDevices = outputDevices,
      .activityStatusViewModel = activityStatusViewModel,
      .events = events,
      .hitRegions = hitRegions,
      .trackColumnLayouts = trackColumnLayouts,
      .trackColumnResizePreview = trackColumnResizePreview,
      .playbackClock = playbackClock,
      .optPreviewElapsed = optPreviewElapsed,
      .coverArt = coverArt,
      .coverArtMode = coverArtDeliveryMode,
      .coverColumns = coverColumns,
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

      if (std::exchange(layoutCheckpointRequested, false))
      {
        saveLayoutState();
      }

      activityStatusViewModel.autoDismissCompactIfDue();

      if (kittyCoverArt)
      {
        updateKittyCoverArt(kittyPaintState, coverArt.resourceId(), hitRegions.coverBox, coverArt.kittyPng());
      }
    }

    if (kittyCoverArt && kittyPaintState.visible)
    {
      std::print("{}", kittyDeleteImageEscape(kKittyCoverArtImageId));
      std::fflush(stdout);
    }

    coverArt.cancel();
    events.cancelTransientInteractions();

    if (layoutStateDirty)
    {
      saveLayoutState();
    }

    runtime.workspace().saveSession(runtime.workspaceConfigStore());

    if (auto const savedRes = runtime.savePlaybackSession(); !savedRes)
    {
      APP_LOG_WARN("TUI: failed to checkpoint the playback session: {}", savedRes.error().message);
    }

    playback.commands().stop();
    frameTimer.flush();
    return 0;
  }
} // namespace ao::tui
