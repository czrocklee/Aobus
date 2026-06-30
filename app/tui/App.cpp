// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "App.h"

#include "AudioBackendBootstrap.h"
#include "CoverArt.h"
#include "EventController.h"
#include "Executor.h"
#include "LibraryController.h"
#include "Model.h"
#include "PlaybackPanel.h"
#include "Render.h"
#include "ShellModel.h"
#include "TrackTable.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/seek/PlaybackTimeViewModel.h>

#include <fcntl.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/terminal.hpp>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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
    std::atomic<std::int32_t> gSignalWriteFd{-1}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    void exitSignalHandler(int /*signal*/) // NOLINT(aobus-modernize-use-std-numbers)
    {
      if (auto const fd = gSignalWriteFd.load(std::memory_order_relaxed); fd >= 0)
      {
        auto const token = std::byte{1};
        [[maybe_unused]] auto const ignored = ::write(fd, &token, sizeof(token));
      }
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

    bool environmentSupportsKittyGraphics()
    {
      auto const* term = std::getenv("TERM");
      auto const* termProgram = std::getenv("TERM_PROGRAM");

      return std::getenv("KITTY_WINDOW_ID") != nullptr || std::getenv("WEZTERM_EXECUTABLE") != nullptr ||
             (term != nullptr && std::string_view{term}.find("xterm-kitty") != std::string_view::npos) ||
             (termProgram != nullptr && std::string_view{termProgram} == "WezTerm");
    }

    bool useKittyCoverArt(CoverArtMode const mode)
    {
      if (mode == CoverArtMode::Kitty)
      {
        return true;
      }

      return mode == CoverArtMode::Auto && environmentSupportsKittyGraphics();
    }

    bool useBlockCoverArt(CoverArtMode const mode)
    {
      return mode == CoverArtMode::Blocks || (mode == CoverArtMode::Auto && !environmentSupportsKittyGraphics());
    }

    bool validBox(ftxui::Box const& box)
    {
      return box.x_max > box.x_min && box.y_max > box.y_min;
    }

    bool sameBox(ftxui::Box const& left, ftxui::Box const& right)
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

    class SignalExitWatcher final
    {
    public:
      explicit SignalExitWatcher(ftxui::ScreenInteractive& screen)
        : _screen{screen}
      {
        if (::pipe(_pipe.data()) != 0)
        {
          return;
        }

        makeWriteEndNonBlocking();
        gSignalWriteFd.store(_pipe[1], std::memory_order_relaxed);
        _termInstalled = install(SIGTERM, _oldTerm);
        _hupInstalled = install(SIGHUP, _oldHup);
        _thread = std::thread{[this] { run(); }};
      }

      ~SignalExitWatcher()
      {
        restore(SIGTERM, _oldTerm, _termInstalled);
        restore(SIGHUP, _oldHup, _hupInstalled);
        gSignalWriteFd.store(-1, std::memory_order_relaxed);
        _running.store(false);
        wake();

        if (_thread.joinable())
        {
          _thread.join();
        }

        closePipe();
      }

      SignalExitWatcher(SignalExitWatcher const&) = delete;
      SignalExitWatcher& operator=(SignalExitWatcher const&) = delete;
      SignalExitWatcher(SignalExitWatcher&&) = delete;
      SignalExitWatcher& operator=(SignalExitWatcher&&) = delete;

    private:
      using SignalAction = struct sigaction;

      static bool install(int const signal, SignalAction& oldAction)
      {
        auto action = SignalAction{};
        action.sa_handler = exitSignalHandler;
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        return ::sigaction(signal, &action, &oldAction) == 0;
      }

      static void restore(int const signal, SignalAction const& oldAction, bool const installed)
      {
        if (installed)
        {
          std::ignore = ::sigaction(signal, &oldAction, nullptr);
        }
      }

      void wake() const
      {
        if (_pipe[1] >= 0)
        {
          auto const token = std::byte{1};
          [[maybe_unused]] auto const ignored = ::write(_pipe[1], &token, sizeof(token));
        }
      }

      void makeWriteEndNonBlocking() const
      {
        if (auto const flags = ::fcntl(_pipe[1], F_GETFL, 0); flags >= 0)
        {
          std::ignore = ::fcntl(_pipe[1], F_SETFL, flags | O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)
        }
      }

      void closePipe()
      {
        for (auto& fd : _pipe)
        {
          if (fd >= 0)
          {
            std::ignore = ::close(fd);
            fd = -1;
          }
        }
      }

      void run()
      {
        while (_running.load())
        {
          auto token = std::byte{};
          auto const result = ::read(_pipe[0], &token, 1);

          if (result > 0)
          {
            if (_running.load())
            {
              _screen.Post(_screen.ExitLoopClosure());
            }

            continue;
          }

          if (result < 0 && errno == EINTR)
          {
            continue;
          }

          break;
        }
      }

      ftxui::ScreenInteractive& _screen;
      std::array<int, 2> _pipe{-1, -1};
      SignalAction _oldTerm{};
      SignalAction _oldHup{};
      bool _termInstalled = false;
      bool _hupInstalled = false;
      std::atomic_bool _running{true};
      std::thread _thread{};
    };

    std::optional<CoverArtRows> loadCoverArtPreview(rt::AppRuntime& runtime, ResourceId const resourceId)
    {
      if (resourceId == kInvalidResourceId)
      {
        return std::nullopt;
      }

      auto reader = runtime.library().reader();
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

      auto reader = runtime.library().reader();
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

    void updateKittyCoverArt(KittyPaintState& state,
                             ShellModel const& shell,
                             ResourceId const cachedCoverArtId,
                             ftxui::Box const& coverBox,
                             std::optional<std::vector<std::byte>> const& optKittyCoverArtPng)
    {
      auto const shouldShow = shell.overlay() == Overlay::DetailPanel && optKittyCoverArtPng && validBox(coverBox);
      auto const shouldPaint = shouldShow && (!state.visible || cachedCoverArtId != state.paintedCoverArtId ||
                                              !sameBox(coverBox, state.paintedCoverBox));

      if (shouldPaint)
      {
        if (state.visible)
        {
          std::cout << kittyDeleteImageEscape(kKittyCoverArtImageId);
        }

        paintKittyCoverArt(coverBox, *optKittyCoverArtPng);
        state.paintedCoverArtId = cachedCoverArtId;
        state.paintedCoverBox = coverBox;
        state.visible = true;
        return;
      }

      if (!shouldShow && state.visible)
      {
        std::cout << kittyDeleteImageEscape(kKittyCoverArtImageId) << std::flush;
        state.visible = false;
        state.paintedCoverArtId = kInvalidResourceId;
        state.paintedCoverBox = {};
      }
    }
  } // namespace

  std::int32_t run(Options const& options)
  {
    auto const coverArtMode = parseCoverArtMode(options.coverArtMode);
    auto const kittyCoverArt = useKittyCoverArt(coverArtMode);
    auto const blockCoverArt = useBlockCoverArt(coverArtMode);

    std::filesystem::create_directories(options.configPath.parent_path());
    rt::Log::init(options.logLevel, options.libraryRoot / ".aobus" / "logs", rt::LogConsoleMode::Disabled);

    auto screen = ftxui::ScreenInteractive::FullscreenAlternateScreen();
    auto runtime = rt::AppRuntime{rt::AppRuntimeDependencies{
      .executorPtr = std::make_unique<Executor>(screen),
      .musicRoot = options.libraryRoot,
      .databasePath = options.databasePath,
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(options.configPath),
    }};

    registerPlatformAudioBackends(runtime);

    auto library = LibraryController{runtime};
    auto shell = ShellModel{};
    auto cachedCoverArtId = kInvalidResourceId;
    auto optCoverArtPreview = std::optional<CoverArtRows>{};
    auto optKittyCoverArtPng = std::optional<std::vector<std::byte>>{};
    auto coverBox = ftxui::Box{};
    auto kittyPaintState = KittyPaintState{};

    auto& playback = runtime.playback();
    auto requestRefresh = [&screen] { screen.PostEvent(ftxui::Event::Custom); };
    auto clockTickActive = std::atomic_bool{transportNeedsClockTick(playback.state().transport)};
    auto playbackClock = uimodel::PlaybackPositionInterpolator{};
    auto optPreviewElapsed = std::optional<std::chrono::milliseconds>{};
    auto playbackTime =
      uimodel::PlaybackTimeViewModel{playback,
                                     [&](uimodel::PlaybackTimeViewState const& view)
                                     {
                                       clockTickActive.store(transportNeedsClockTick(playback.state().transport));

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

    auto nowPlayingSub = playback.onNowPlayingChanged([requestRefresh](auto const&) { requestRefresh(); });
    auto qualitySub = playback.onQualityChanged([requestRefresh](auto const&) { requestRefresh(); });
    auto volumeSub = playback.onVolumeChanged([requestRefresh](float) { requestRefresh(); });
    auto mutedSub = playback.onMutedChanged([requestRefresh](bool) { requestRefresh(); });
    auto events = EventController{screen, shell, library, playback};

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
          coverBox = ftxui::Box{};
        }

        auto coverElementPtr =
          kittyCoverArt ? renderKittyCoverArtPlaceholder(optKittyCoverArtPng != std::nullopt) | reflect(coverBox)
                        : renderCoverArtPreview(optCoverArtPreview) | reflect(coverBox);

        auto const currentListTitle = library.currentListTitle();
        auto const state = playback.state();
        auto const displayElapsed = optPreviewElapsed.value_or(playbackClock.interpolateElapsed(monotonicFrameTime()));
        auto const viewState = runtime.views().trackListState(library.activeViewId());
        auto const terminalColumns = ftxui::Terminal::Size().dimx;
        auto workspaceElementPtr = trackTableView(library.tracks(), library.selectedTrack(), state.trackId);
        auto mainContentPtr = workspaceElementPtr;
        auto popoverElementPtr = ftxui::Element{};

        switch (shell.overlay())
        {
          case Overlay::None: break;
          case Overlay::ListChooser:
            popoverElementPtr = bottomPopover(libraryChooserPane(library.libraryLabels(), library.selectedList()));
            break;
          case Overlay::DetailPanel:
            mainContentPtr = hbox({
              workspaceElementPtr,
              detailPane(selectedTrackView.track, std::move(coverElementPtr)),
            });
            break;
          case Overlay::QualityPanel: popoverElementPtr = topPopover(qualityPanel(state)); break;
          case Overlay::Help:
            mainContentPtr = hbox({
              workspaceElementPtr,
              helpPane(),
            });
            break;
        }

        auto mainLayerPtr = popoverElementPtr == nullptr ? std::move(mainContentPtr)
                                                         : dbox({
                                                             std::move(mainContentPtr),
                                                             std::move(popoverElementPtr),
                                                           });

        return vbox({
          playbackBar(state, currentListTitle, displayElapsed),
          text(""),
          std::move(mainLayerPtr) | flex,
          text(""),
          statusBar(StatusBarViewState{.statusMessage = events.statusMessage(),
                                       .trackCount = library.tracks().size(),
                                       .selectedTrack = library.selectedTrack(),
                                       .filterDraft = library.filterDraft(),
                                       .presentationId = viewState.presentation.id,
                                       .shell = &shell,
                                       .terminalColumns = terminalColumns}),
        });
      });

    auto componentPtr =
      ftxui::CatchEvent(rendererPtr, [&](ftxui::Event const& event) { return events.handleEvent(event); });

    auto loop = ftxui::Loop{&screen, componentPtr};
    auto signalExit = SignalExitWatcher{screen};
    auto refreshTick =
      PeriodicRefresh{screen, kPlaybackTickInterval, [&clockTickActive] { return clockTickActive.load(); }};

    while (!loop.HasQuitted())
    {
      loop.RunOnceBlocking();

      if (kittyCoverArt)
      {
        updateKittyCoverArt(kittyPaintState, shell, cachedCoverArtId, coverBox, optKittyCoverArtPng);
      }
    }

    if (kittyCoverArt && kittyPaintState.visible)
    {
      std::cout << kittyDeleteImageEscape(kKittyCoverArtImageId) << std::flush;
    }

    playback.stop();
    runtime.async().requestStop();
    runtime.async().join();
    rt::Log::shutdown();
    return 0;
  }
} // namespace ao::tui
