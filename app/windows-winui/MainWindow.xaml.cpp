// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"

#include "app/LibrarySession.h"
#include "app/UiCoordinator.h"
// The window's own destructor destroys the group-cover presenters it retains.
#include "image/CoverArtPresenter.h" // NOLINT(misc-include-cleaner)
#include "layout/ShellBuilder.h"
#include "library/LibraryTransferCoordinator.h"
#include "list/ListAuthoringCoordinator.h"
#include "pch.h"
// MainWindow's out-of-line destructor destroys the retained SMTC bridge.
#include "platform/SmtcBridge.h" // NOLINT(misc-include-cleaner)
#include "platform/StringResources.h"
#include "playback/OutputDeviceControl.h"
#include "theme/ThemeCoordinator.h"
#include "track/TrackPropertiesCoordinator.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
// MainWindow's out-of-line destructor requires the unique_ptr target to be complete.
#include <ao/uimodel/library/list/ListMembershipAuthoringSession.h>
#include <ao/uimodel/library/list/ListOrderPolicy.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h> // NOLINT(misc-include-cleaner)
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/WindowInteractionPolicy.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
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
#include <vector>

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
          self->updateStatus(ao::winui::formatResource("winui_error", error.message));
        }
      },
    };
    _coordinatorPtr = std::make_unique<ao::winui::UiCoordinator>(session, std::move(callbacks));
    _trackListPtr = &_coordinatorPtr->trackList();
    _resourceBytes = &_coordinatorPtr->resourceBytes();
    _themePtr = &_coordinatorPtr->theme();

    auto& runtime = session.runtime();
    _listAuthoringCoordinatorPtr =
      std::make_unique<ao::winui::ListAuthoringCoordinator>(ao::winui::ListAuthoringCoordinatorConfig{
        .xamlRoot =
          [weak]
        {
          if (auto self = weak.get(); self)
          {
            return self->RootGrid().XamlRoot();
          }

          return Microsoft::UI::Xaml::XamlRoot{nullptr};
        },
        .asyncRuntime = runtime.async(),
        .library = runtime.library(),
        .views = runtime.views(),
        .sources = runtime.sources(),
        .trackList = *_trackListPtr,
        .presentationCatalog = session.presentationCatalog(),
        .presentationPreferences = session.presentationPreferenceStore(),
        .textOrderingPolicy = runtime.textOrderingPolicy(),
        .textCatalog = session.textCatalog(),
        .reportStatus =
          [weak](std::string status)
        {
          if (auto self = weak.get(); self)
          {
            self->updateStatus(status);
          }
        },
      });
    _libraryTransferCoordinatorPtr =
      std::make_unique<ao::winui::LibraryTransferCoordinator>(ao::winui::LibraryTransferCoordinatorConfig{
        .xamlRoot =
          [weak]
        {
          if (auto self = weak.get(); self)
          {
            return self->RootGrid().XamlRoot();
          }

          return Microsoft::UI::Xaml::XamlRoot{nullptr};
        },
        .windowId = AppWindow().Id(),
        .asyncRuntime = runtime.async(),
        .taskService = runtime.library().taskService(),
        .notifications = runtime.notifications(),
        .textCatalog = session.textCatalog(),
        .reportStatus =
          [weak](std::string status)
        {
          if (auto self = weak.get(); self)
          {
            self->updateStatus(std::move(status));
          }
        },
      });

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
    updateStatus(ao::winui::resourceString("winui_library_ready"));
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

  bool MainWindow::modalWorkflowActive() const noexcept
  {
    return ao::winui::hasActiveWindowModalWorkflow(ao::winui::WindowModalWorkflowState{
      .listAuthoring = _listAuthoringCoordinatorPtr && _listAuthoringCoordinatorPtr->dialogActive(),
      .libraryTransfer = _libraryTransferCoordinatorPtr && _libraryTransferCoordinatorPtr->active(),
      .trackProperties = _trackPropertiesCoordinatorPtr && _trackPropertiesCoordinatorPtr->active(),
    });
  }

  void MainWindow::navigateHistory(bool const forward)
  {
    if (_session == nullptr || modalWorkflowActive())
    {
      return;
    }

    auto& workspace = _session->runtime().workspace();

    if ((forward && !workspace.canGoForward()) || (!forward && !workspace.canGoBack()))
    {
      return;
    }

    auto const navigatedRes = forward ? workspace.goForward() : workspace.goBack();

    if (!navigatedRes)
    {
      updateStatus(ao::winui::formatResource("winui_navigation_failed", navigatedRes.error().message));
    }
  }

  void MainWindow::OnNavigateBackInvoked(Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
                                         Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
  {
    navigateHistory(false);
    args.Handled(true);
  }

  void MainWindow::OnNavigateForwardInvoked(Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
                                            Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
  {
    navigateHistory(true);
    args.Handled(true);
  }

  void MainWindow::OnRootPointerPressed(Windows::Foundation::IInspectable const& /*sender*/,
                                        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
  {
    auto const kind = args.GetCurrentPoint(RootGrid()).Properties().PointerUpdateKind();

    if (kind == Microsoft::UI::Input::PointerUpdateKind::XButton1Pressed)
    {
      navigateHistory(false);
      args.Handled(true);
    }
    else if (kind == Microsoft::UI::Input::PointerUpdateKind::XButton2Pressed)
    {
      navigateHistory(true);
      args.Handled(true);
    }
  }

  // This declarative composition keeps the shell's callback ownership visible
  // in one place; splitting individual callbacks would obscure that boundary.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void MainWindow::createShellBuilder()
  {
    // The selector belongs to the frame rather than to a generation: the node
    // that raises it only names an action, and the menu it opens has to outlive
    // whichever shell was on screen when it was asked for.
    auto weak = get_weak();
    _shellOutputDevicePtr = std::make_unique<ao::winui::OutputDeviceControl>(ao::winui::OutputDeviceControlConfig{
      .intent = ao::uimodel::OutputDeviceIntent::recordedBy(
        [weak](ao::audio::OutputDeviceSelection const& selection)
        {
          if (auto self = weak.get(); self && self->_session != nullptr)
          {
            self->_session->setPreferredOutputSelection(selection);
          }
        }),
      .textCatalog = _session->textCatalog(),
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
          .importLibrary = command(&MainWindow::importLibrary),
          .exportLibrary = command(&MainWindow::exportLibrary),
          .toggleInspector = command(&MainWindow::toggleInspector),
          .toggleShellMode = command(&MainWindow::toggleShellMode),
          .chooseColumns = command(&MainWindow::showColumnsMenu),
          .reloadTheme = command(&MainWindow::reloadTheme),
          .playPause = command(&MainWindow::playPause),
          .stop = command(&MainWindow::stopPlayback),
          .revealCurrentTrack = command(&MainWindow::revealCurrentTrack),
          .presentTrackProperties = command(&MainWindow::presentTrackProperties),
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
        .listCommands = {
          .createList =
            [weak](ao::ListId const parentListId, std::string expression)
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->modalWorkflowActive())
            {
              self->_listAuthoringCoordinatorPtr->createList(parentListId, std::move(expression));
            }
          },
          .editList =
            [weak](ao::ListId const listId)
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->modalWorkflowActive())
            {
              self->_listAuthoringCoordinatorPtr->editList(listId);
            }
          },
          .deleteList =
            [weak](ao::ListId const listId, bool const includeDescendants)
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->modalWorkflowActive())
            {
              self->_listAuthoringCoordinatorPtr->deleteList(listId, includeDescendants);
            }
          },
          .membershipTargets =
            [weak]
          {
            if (auto self = weak.get(); self && self->_listAuthoringCoordinatorPtr)
            {
              return self->_listAuthoringCoordinatorPtr->membershipTargets();
            }

            return std::vector<ao::uimodel::WritableTagListTarget>{};
          },
          .editMembership =
            [weak](ao::ListId const listId, bool const add)
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->modalWorkflowActive())
            {
              self->_listAuthoringCoordinatorPtr->editMembership(listId, add);
            }
          },
          .orderCapabilities =
            [weak]
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->modalWorkflowActive())
            {
              return self->_listAuthoringCoordinatorPtr->orderCapabilities();
            }

            return ao::uimodel::ListOrderCapabilityState{};
          },
          .applyOrder =
            [weak](ao::winui::ListOrderCommand const commandValue)
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->modalWorkflowActive())
            {
              self->_listAuthoringCoordinatorPtr->applyOrder(commandValue);
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

    // Cancel authoring and close its transient native tree before releasing
    // either the shell selection that opened it or the runtime it submits to.
    _trackPropertiesCoordinatorPtr.reset();

    // Generation callbacks and all generation-owned runtime borrowers go away
    // before the coordinator/resource loader is released. Releasing the owner
    // is the retirement: each destructor runs its own quiescence, so the order
    // of these resets is the whole contract.
    _shellBuilderPtr.reset();
    _libraryTransferCoordinatorPtr.reset();
    _listAuthoringCoordinatorPtr.reset();

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
      updateStatus(ao::winui::formatResource("winui_library_switch_failed", retiredRes.error().message));
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
