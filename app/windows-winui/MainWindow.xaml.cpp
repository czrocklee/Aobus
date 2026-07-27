// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"

#include "app/LibrarySession.h"
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
#include "track/TrackRowItem.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/shell/DesktopShellPolicy.h>
#include <ao/uimodel/layout/shell/WindowsDesktopSettingsYamlSchema.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/preference/WindowsTheme.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>
#include <ao/utility/Path.h>

#include <microsoft.ui.xaml.window.h>
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    _coordinatorPtr =
      std::make_unique<ao::winui::WindowsUiCoordinator>(session,
                                                        ao::winui::WindowsUiViewDependencies{
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
                                                              .classicMetadataHeaderButton =
                                                                ClassicMetadataHeaderButton(),
                                                              .classicMetadataHeader = ClassicMetadataHeader(),
                                                              .classicMetadataChevron = ClassicMetadataChevron(),
                                                              .classicMetadataRows = ClassicMetadataRows(),
                                                              .classicShowEmptyButton = ClassicShowEmpty(),
                                                              .classicTechnicalSection = ClassicTechnicalSection(),
                                                              .classicTechnicalHeaderButton =
                                                                ClassicTechnicalHeaderButton(),
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
                                                        },
                                                        ao::
                                                          winui::WindowsUiCoordinatorCallbacks{.onTrackListChanged =
                                                                                                 [weak]
                                                                                               {
                                                                                                 if (auto self =
                                                                                                       weak.get())
                                                                                                 {
                                                                                                   self
                                                                                                     ->updateBrowserHeader();
                                                                                                 }
                                                                                               },
                                                                                               .onLibraryChanged =
                                                                                                 [weak]
                                                                                               {
                                                                                                 if (auto self =
                                                                                                       weak.get())
                                                                                                 {
                                                                                                   self
                                                                                                     ->reconcileLibrary();
                                                                                                 }
                                                                                               },
                                                                                               .onPlaybackChanging =
                                                                                                 [weak]
                                                                                               {
                                                                                                 if (auto self =
                                                                                                       weak.get())
                                                                                                 {
                                                                                                   self
                                                                                                     ->unbindPlayback();
                                                                                                 }
                                                                                               },
                                                                                               .onPlaybackChanged =
                                                                                                 [weak]
                                                                                               {
                                                                                                 if (auto self =
                                                                                                       weak.get())
                                                                                                 {
                                                                                                   self->bindPlayback();
                                                                                                 }
                                                                                               },
                                                                                               .onStatus =
                                                                                                 [weak](
                                                                                                   std::string status)
                                                                                               {
                                                                                                 if (auto self =
                                                                                                       weak.get())
                                                                                                 {
                                                                                                   self->updateStatus(
                                                                                                     status);
                                                                                                 }
                                                                                               },
                                                                                               .onFailure =
                                                                                                 [weak](ao::Error const&
                                                                                                          error)
                                                                                               {
                                                                                                 if (auto self =
                                                                                                       weak.get())
                                                                                                 {
                                                                                                   self->updateStatus(
                                                                                                     ao::winui::
                                                                                                       formatResource(
                                                                                                         "ErrorFormat",
                                                                                                         error
                                                                                                           .message));
                                                                                                 }
                                                                                               }});
    auto const dependencies = _coordinatorPtr->uiDependencies();
    _trackListPtr = &dependencies.trackList;
    _nowPlayingCoverArtPtr = &dependencies.nowPlayingCoverArt;
    _themePtr = &dependencies.theme;

    reconcileLibrary();
    bindPlayback();
    restoreWindowPlacement();
    applyShellState(RootGrid().ActualWidth());
    if (auto theme = _themePtr->reload())
    {
      applyTheme(*theme);
    }
    ModernLibraryPath().Text(to_hstring(ao::utility::pathToUtf8(session.libraryRuntime().musicRoot())));
    updateStatus(ao::winui::resourceString("Ready"));
  }

  void MainWindow::shutdown()
  {
    if (_shutdown)
    {
      return;
    }
    _shutdown = true;

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
