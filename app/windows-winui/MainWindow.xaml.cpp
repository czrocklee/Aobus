// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"

#include "app/LibrarySession.h"
#include "app/UiCoordinator.h"
// The window's own destructor destroys the group-cover presenters it retains.
#include "image/CoverArtPresenter.h" // NOLINT(misc-include-cleaner)
#include "layout/ShellBuilder.h"
#include "pch.h"
// MainWindow's out-of-line destructor destroys the retained SMTC bridge.
#include "platform/SmtcBridge.h" // NOLINT(misc-include-cleaner)
#include "platform/StringResources.h"
#include "playback/OutputDeviceControl.h"
#include "theme/ThemeCoordinator.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>
// MainWindow's out-of-line destructor requires the unique_ptr target to be complete.
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h> // NOLINT(misc-include-cleaner)
#include <ao/winui/WinUiErrorBoundary.h>

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

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace winrt::Aobus::implementation
{
  MainWindow::MainWindow()
  {
    InitializeComponent();
    Title(ao::winui::resourceHstring(L"AppTitleValue"));

    ao::winui::runOptionalWinRt(
      "enabling the Mica backdrop", [this] { SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{}); });

    _appWindowChangedSub = subscribeAppWindowChanges(AppWindow(), &MainWindow::updateSoulWindowActivity);
    updateSoulWindowActivity();
  }

  MainWindow::~MainWindow()
  {
    shutdown();
  }

  ao::utility::ScopedRegistration MainWindow::subscribeAppWindowChanges(Microsoft::UI::Windowing::AppWindow window,
                                                                        void (MainWindow::* const updateActivity)())
  {
    auto const weak = get_weak();
    auto const token = window.Changed(
      [weak, updateActivity](
        Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowChangedEventArgs const& args)
      {
        if (args.DidVisibilityChange() || args.DidPresenterChange())
        {
          if (auto self = weak.get(); self)
          {
            ((*self).*updateActivity)();
          }
        }
      });

    // AppWindow does not implement IWeakReferenceSource, so its auto_revoke
    // overload crashes while constructing the revoker. Keep the source alive
    // in the cleanup closure and use its noexcept event removal overload.
    return ao::utility::ScopedRegistration{[window = std::move(window), token] noexcept { window.Changed(token); }};
  }

  void MainWindow::initialize(ao::winui::LibrarySession& session, RestartLibraryCallback requestRestart)
  {
    _session = &session;
    _requestRestart = std::move(requestRestart);
    auto weak = get_weak();
    auto callbacks = ao::winui::UiCoordinatorCallbacks{
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
    _coordinatorPtr = std::make_unique<ao::winui::UiCoordinator>(session, std::move(callbacks));
    _trackListPtr = &_coordinatorPtr->trackList();
    _resourceBytes = &_coordinatorPtr->resourceBytes();
    _themePtr = &_coordinatorPtr->theme();

    if (auto themeRes = _themePtr->reload(); themeRes)
    {
      applyTheme(*themeRes);
    }

    // Built before the first resolved policy, which is what asks it for a shell.
    createShellBuilder();
    updateSoulWindowActivity();
    reconcileLibrary();
    restoreWindowPlacement();
    applyShellState(RootGrid().ActualWidth());
    updateStatus(ao::winui::resourceString("Ready"));
    _sessionPhase = SessionPhase::Prepared;
  }

  ao::Result<> MainWindow::activate()
  {
    if (_sessionPhase != SessionPhase::Prepared)
    {
      return ao::makeError(ao::Error::Code::InvalidState, "Only a prepared WinUI session can be activated");
    }

    try
    {
      // SMTC and the frame's process-visible playback adapters are activated
      // only after the native window has been activated by its session owner.
      bindPlayback();
      _sessionPhase = SessionPhase::Active;
      return {};
    }
    catch (winrt::hresult_error const& error)
    {
      return ao::makeError(
        ao::Error::Code::InitFailed,
        std::format("Failed to activate WinUI playback adapters: {}", winrt::to_string(error.message())));
    }
  }

  void MainWindow::createShellBuilder()
  {
    // The selector belongs to the frame rather than to a generation: the node
    // that raises it only names an action, and the menu it opens has to outlive
    // whichever shell was on screen when it was asked for.
    auto weak = get_weak();
    _shellOutputDevicePtr = std::make_unique<ao::winui::OutputDeviceControl>(ao::winui::OutputDeviceControlConfig{
      .onSelectionRequested =
        [weak](ao::audio::OutputDeviceSelection const& selection)
      {
        if (auto self = weak.get(); self && self->_session != nullptr)
        {
          self->_session->setPreferredOutputSelection(selection);
        }
      },
    });

    auto const command = [weak](void (MainWindow::*method)())
    {
      return [weak, method]
      {
        if (auto self = weak.get(); self)
        {
          ((*self).*method)();
        }
      };
    };

    _shellBuilderPtr = std::make_unique<ao::winui::layout::ShellBuilder>(
      *_session,
      ao::winui::layout::ShellBuilderConfig{
        .host = ShellLayoutHost(),
        .resources = RootGrid().Resources(),
        // The window owns the builder, so these are reached only from a build
        // the window itself started while it still owns the coordinator.
        .trackList = _coordinatorPtr->trackList(),
        .resourceBytes = _coordinatorPtr->resourceBytes(),
        .theme = _coordinatorPtr->theme(),
        .activeTheme = [this] { return _themeOverride; },
        .saveSettings = command(&MainWindow::saveWindowState),
        .commands = {
          .openLibrary =
            [weak]
          {
            if (auto self = weak.get(); self)
            {
              self->pickLibrary();
            }
          },
          .rescanLibrary = command(&MainWindow::rescanLibrary),
          .toggleInspector = command(&MainWindow::toggleInspector),
          .toggleShellMode = command(&MainWindow::toggleShellMode),
          .chooseColumns = command(&MainWindow::showColumnsMenu),
          .reloadTheme = command(&MainWindow::reloadTheme),
          .playPause = command(&MainWindow::playPause),
          .stop = command(&MainWindow::stopPlayback),
          .showSoul = command(&MainWindow::showFullscreenSoul),
          .showSystemMenu = command(&MainWindow::showSystemMenu),
          .showOutputDeviceSelector =
            [weak](Microsoft::UI::Xaml::FrameworkElement const& anchor)
          {
            if (auto self = weak.get(); self)
            {
              self->showOutputDeviceSelector(anchor);
            }
          },
        },
      });
  }

  void MainWindow::shutdown() noexcept
  {
    if (_sessionPhase == SessionPhase::Retired)
    {
      return;
    }

    // Marked retired up front, so a callback that re-enters during teardown
    // turns around at the guard above. Retirement is still not terminal in the
    // other sense: every step below is independent, so a native revocation
    // failure cannot suppress the remaining quiescence.
    _sessionPhase = SessionPhase::Retired;
    _requestRestart = {};

    _appWindowChangedSub.reset();
    clearGroupCoverPresenters();

    // Generation callbacks and all generation-owned runtime borrowers go away
    // before the coordinator/resource loader is released. Releasing the owner
    // is the retirement: each destructor runs its own quiescence, so the order
    // of these resets is the whole contract.
    _shellBuilderPtr.reset();

    // SMTC and fullscreen playback adapters must stop observing the runtime
    // before the coordinator releases its resource and track services.
    unbindPlayback();
    _coordinatorPtr.reset();

    _soulWindowChangedSub.reset();

    if (_soulWindow)
    {
      _soulWindow.Close();

      _soulWindow = nullptr;
    }

    _fullscreenSoul = nullptr;
    _shellOutputDevicePtr.reset();
    _smtcPtr.reset();
    _trackListPtr = nullptr;
    _resourceBytes = nullptr;
    _themePtr = nullptr;
    _session = nullptr;
  }

  ao::Result<> MainWindow::prepareLibraryRestart()
  {
    if (_sessionPhase != SessionPhase::Active || _session == nullptr)
    {
      return ao::makeError(ao::Error::Code::InvalidState, "The WinUI library window is not active");
    }

    saveWindowState();
    auto retiredRes = _session->retirePlaybackSessionForLibrarySwitch();

    if (!retiredRes)
    {
      updateStatus(ao::winui::formatResource("LibrarySwitchFailedFormat", retiredRes.error().message));
    }

    return retiredRes;
  }

  void MainWindow::retire() noexcept
  {
    if (_sessionPhase == SessionPhase::Active)
    {
      saveWindowState();
    }

    shutdown();
  }
} // namespace winrt::Aobus::implementation
