// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"

#include "app/LibrarySession.h"
#include "app/WinUiDependencies.h"
#include "app/WindowsUiCoordinator.h"
#include "image/CoverArtPresenter.h"
#include "pch.h"
#include "platform/SmtcBridge.h"
#include "platform/WindowsStringResources.h"
#include "playback/AobusSoulControl.h"
#include "playback/AudioPipelineToolTip.h"
#include "playback/PlaybackControls.h"
#include "theme/WindowsThemeCoordinator.h"
#include "track/TrackListController.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
// MainWindow's out-of-line destructor requires the unique_ptr target to be complete.
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h> // NOLINT(misc-include-cleaner)
#include <ao/utility/Path.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace winrt::Aobus::implementation
{
  namespace
  {
    constexpr double kModernSoulStrokeWidth = 5.0;
    constexpr double kModernSoulGlyphScale = 0.85;
  } // namespace

  MainWindow::MainWindow()
  {
    InitializeComponent();
    get_self<AobusSoulControl>(ModernSoul())->setBaseStrokeWidth(kModernSoulStrokeWidth);
    get_self<AobusSoulControl>(ModernSoul())->setInnerGlyphScale(kModernSoulGlyphScale);
    Title(ao::winui::resourceHstring(L"AppTitleValue"));

    try
    {
      SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
    }
    // Mica is optional on remote or composition-disabled desktops.
    // NOLINTNEXTLINE(bugprone-empty-catch)
    catch (hresult_error const&)
    {
      // Solid theme resources remain the supported fallback on remote or
      // composition-disabled desktops.
    }

    applyShellState(RootGrid().ActualWidth());

    _appWindowChangedToken = AppWindow().Changed(
      [this](
        Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowChangedEventArgs const& args)
      {
        if (args.DidVisibilityChange() || args.DidPresenterChange())
        {
          updateSoulWindowActivity();
        }
      });
    _hasAppWindowChangedToken = true;
    _closedToken = Closed(
      [this](Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::WindowEventArgs const&)
      {
        if (_hasAppWindowChangedToken)
        {
          AppWindow().Changed(_appWindowChangedToken);
          _hasAppWindowChangedToken = false;
        }

        if (_hasClosedToken)
        {
          Closed(_closedToken);
          _hasClosedToken = false;
        }

        saveWindowState();
        shutdown();
      });
    _hasClosedToken = true;
    updateSoulWindowActivity();
  }

  MainWindow::~MainWindow()
  {
    if (_hasAppWindowChangedToken)
    {
      AppWindow().Changed(_appWindowChangedToken);
      _hasAppWindowChangedToken = false;
    }

    if (_hasClosedToken)
    {
      Closed(_closedToken);
      _hasClosedToken = false;
    }

    shutdown();
  }

  void MainWindow::initialize(ao::winui::LibrarySession& session)
  {
    _session = &session;
    _playbackControlsPtr = std::make_unique<ao::winui::PlaybackControls>(ao::winui::PlaybackControlsConfig{
      .modern =
        {
          .shuffleButton = ModernShuffleButton(),
          .previousButton = ModernPreviousButton(),
          .outputButton = ModernOutputButton(),
          .soulButton = ModernSoulButton(),
          .soul = ModernSoul().as<Microsoft::UI::Xaml::Controls::ContentControl>(),
          .nextButton = ModernNextButton(),
          .repeatButton = ModernRepeatButton(),
          .seek = ModernSeek(),
          .elapsed = ModernElapsed(),
          .duration = ModernDuration(),
          .volume = ModernVolume(),
        },
      .classic =
        {
          .previousButton = ClassicPreviousButton(),
          .nextButton = ClassicNextButton(),
          .soulButton = ClassicSoulButton(),
          .playPauseButton = ClassicPlayPauseButton(),
          .stopButton = ClassicStopButton(),
          .seek = ClassicSeek(),
          .time = ClassicTime(),
          .volume = ClassicVolume(),
        },
    });
    _audioPipelineToolTipPtr = std::make_unique<ao::winui::AudioPipelineToolTip>(ao::winui::AudioPipelineToolTipConfig{
      .modernAnchor = ModernSoulButton(),
      .classicAnchor = ClassicSoulButton(),
    });

    auto weak = get_weak();
    auto views = ao::winui::WindowsUiViewDependencies{
      .quickFilterInput = ModernFilter(),
      .inspectorCoverImage = InspectorCoverImage(),
      .inspectorCoverPlaceholder = InspectorCoverPlaceholder(),
      .trackDetail =
        {
          .fieldScroll = InspectorFieldScroll(),
          .detailContent = InspectorDetailContent(),
          .metadataHeaderButton = InspectorMetadataHeaderButton(),
          .metadataHeader = InspectorMetadataHeader(),
          .metadataChevron = InspectorMetadataChevron(),
          .metadataRows = InspectorMetadataRows(),
          .showEmptyButton = InspectorShowEmpty(),
          .technicalHeaderButton = InspectorTechnicalHeaderButton(),
          .technicalHeader = InspectorTechnicalHeader(),
          .technicalChevron = InspectorTechnicalChevron(),
          .technicalRows = InspectorTechnicalRows(),
          .classicFieldScroll = ClassicDetailScroll(),
          .classicDetailContent = ClassicDetailContent(),
          .classicMetadataSection = ClassicMetadataSection(),
          .classicMetadataHeaderButton = ClassicMetadataHeaderButton(),
          .classicMetadataHeader = ClassicMetadataHeader(),
          .classicMetadataChevron = ClassicMetadataChevron(),
          .classicMetadataRows = ClassicMetadataRows(),
          .classicShowEmptyButton = ClassicShowEmpty(),
          .classicTechnicalSection = ClassicTechnicalSection(),
          .classicTechnicalHeaderButton = ClassicTechnicalHeaderButton(),
          .classicTechnicalHeader = ClassicTechnicalHeader(),
          .classicTechnicalChevron = ClassicTechnicalChevron(),
          .classicTechnicalRows = ClassicTechnicalRows(),
        },
      .activityStatus =
        {
          .root = ModernActivityStatus(),
          .detailButton = ModernActivityDetailButton(),
          .spinner = ModernActivitySpinner(),
          .statusIcon = ModernActivityStatusIcon(),
          .label = ModernActivityLabel(),
          .progress = ModernActivityProgress(),
          .dismissButton = ModernActivityDismissButton(),
          .reserveIdle = true,
        },
      .nowPlayingCoverImage = NowPlayingCoverImage(),
      .nowPlayingCoverPlaceholder = NowPlayingCoverPlaceholder(),
    };
    auto callbacks = ao::winui::WindowsUiCoordinatorCallbacks{
      .onTrackListChanged =
        [weak]
      {
        if (auto self = weak.get(); self)
        {
          self->updateBrowserHeader();
        }
      },
      .onRuntimeChanging =
        [weak] noexcept
      {
        if (auto self = weak.get(); self)
        {
          self->clearGroupCoverPresenters();
          self->unbindPlayback();
        }
      },
      .onRuntimeChanged =
        [weak] noexcept
      {
        if (auto self = weak.get(); self)
        {
          self->reconcileLibrary();
          self->bindPlayback();
        }
      },
      .onStatus =
        [weak](std::string status)
      {
        if (auto self = weak.get(); self)
        {
          self->updateStatus(status);
        }
      },
      .onFailure =
        [weak](ao::Error const& error)
      {
        if (auto self = weak.get(); self)
        {
          self->updateStatus(ao::winui::formatResource("ErrorFormat", error.message));
        }
      },
    };
    _coordinatorPtr =
      std::make_unique<ao::winui::WindowsUiCoordinator>(session, std::move(views), std::move(callbacks));
    auto const dependencies = _coordinatorPtr->uiDependencies();
    _trackListPtr = &dependencies.trackList;
    _resourceBytes = &dependencies.resourceBytes;
    _nowPlayingCoverArtPtr = &dependencies.nowPlayingCoverArt;
    _themePtr = &dependencies.theme;

    if (auto theme = _themePtr->reload(); theme)
    {
      applyTheme(*theme);
    }

    reconcileLibrary();
    bindPlayback();
    restoreWindowPlacement();
    applyShellState(RootGrid().ActualWidth());
    ModernLibraryPath().Text(to_hstring(ao::utility::pathToUtf8(session.runtime().musicRoot())));
    updateStatus(ao::winui::resourceString("Ready"));
  }

  void MainWindow::shutdown()
  {
    if (_shutdown)
    {
      return;
    }

    _shutdown = true;

    clearGroupCoverPresenters();

    if (_coordinatorPtr)
    {
      _coordinatorPtr->retire();
    }

    if (_smtcPtr)
    {
      _smtcPtr->unbind();
    }

    if (_fullscreenSoul)
    {
      get_self<AobusSoulControl>(_fullscreenSoul)->unbind();
    }

    if (_soulWindow)
    {
      if (_hasSoulWindowChangedToken)
      {
        _soulWindow.AppWindow().Changed(_soulWindowChangedToken);
        _hasSoulWindowChangedToken = false;
      }

      _soulWindow.Close();
      _soulWindow = nullptr;
      _fullscreenSoul = nullptr;
    }

    unbindPlayback();
    _smtcPtr.reset();
    _audioPipelineToolTipPtr.reset();
    _playbackControlsPtr.reset();
    _trackListPtr = nullptr;
    _resourceBytes = nullptr;
    _nowPlayingCoverArtPtr = nullptr;
    _themePtr = nullptr;
    _coordinatorPtr.reset();
    _session = nullptr;
  }

  void MainWindow::retire()
  {
    shutdown();
  }
} // namespace winrt::Aobus::implementation
