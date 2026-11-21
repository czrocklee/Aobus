// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"

// Keep the Win32 macro out of subsequently included XAML projections.
#undef GetCurrentTime

#include "app/LibrarySession.h"
#include "app/LibraryWindowSession.h"
#include "image/GroupCoverPresenters.h"
#include "layout/ShellBuilder.h"
#include "library/LibraryTransferCoordinator.h"
#include "list/ListAuthoringCoordinator.h"
#include "platform/AppWindowActivity.h"
#include "platform/NativeWindow.h"
#include "platform/StringResourceFormat.h"
#include "platform/StringResources.h"
#include "platform/WinRtStringResources.h"
#include "playback/FullscreenSoulWindow.h"
#include "playback/OutputDeviceControl.h"
#include "theme/SurfaceBrushes.h"
#include "theme/ThemeCoordinator.h"
#include "track/TrackListController.h"
#include "track/TrackPropertiesPresenter.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/utility/Path.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/Theme.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/layout/ShellState.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <winrt/Aobus.h>
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
#include <winrt/base.h>

#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

namespace winrt::Aobus::implementation
{
  MainWindow::MainWindow()
  {
    InitializeComponent();
    Title(ao::winui::resourceHstring(L"AppTitleValue"));

    ao::winui::runOptionalWinRt(
      "enabling the Mica backdrop", [this] { SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{}); });

    _appWindowChangedSub = ao::winui::observeWindowActivityChanges(AppWindow(),
                                                                   [weak = get_weak()]
                                                                   {
                                                                     if (auto self = weak.get(); self)
                                                                     {
                                                                       self->updateSoulWindowActivity();
                                                                     }
                                                                   });
    updateSoulWindowActivity();
  }

  MainWindow::~MainWindow()
  {
    shutdown();
  }

  void MainWindow::initialize(ao::winui::LibrarySession& session, RestartLibraryCallback requestRestart)
  {
    _session = &session;
    _requestRestart = std::move(requestRestart);
    auto weak = get_weak();
    auto& runtime = session.runtime();

    // The window's own runtime consumers. They outlive every shell generation
    // and every dialog the window opens, which is why the window owns them
    // rather than borrowing them from a layer above or below it.
    _fullscreenSoulWindowPtr = std::make_unique<ao::winui::FullscreenSoulWindow>(runtime.playback());
    _trackListPtr = std::make_unique<ao::winui::TrackListController>(runtime.views(),
                                                                     runtime.workspace(),
                                                                     runtime.library(),
                                                                     session.columnLayouts(),
                                                                     session.listPresentations(),
                                                                     session.textCatalog());
    _themePtr = std::make_unique<ao::winui::ThemeCoordinator>(session.stateRoot() / "windows-theme.yaml");
    _groupCoverPresentersPtr =
      std::make_unique<ao::winui::GroupCoverPresenters>(runtime.async(), runtime.resourceBytes(), *_themePtr);

    // A reveal request reaches the track list the window owns, not the list a
    // generation happens to be showing, so it survives every shell rebuild.
    _revealTrackSub = runtime.playback().events().onRevealTrackRequested(
      [weak](ao::rt::PlaybackRevealTrackRequest const& request)
      {
        auto self = weak.get();

        if (!self || !self->_trackListPtr)
        {
          return;
        }

        if (auto const revealedRes =
              self->_trackListPtr->revealTrack(request.trackId, request.preferredViewId, request.preferredListId);
            !revealedRes)
        {
          self->updateStatus(ao::winui::formatResource("winui_error", revealedRes.error().message));
        }
      });

    auto const currentRoot = [weak]
    {
      if (auto self = weak.get(); self)
      {
        return self->RootGrid().XamlRoot();
      }

      return Microsoft::UI::Xaml::XamlRoot{nullptr};
    };
    auto const reportStatus = [weak](std::string status)
    {
      if (auto self = weak.get(); self)
      {
        self->updateStatus(status);
      }
    };

    // The window reports into whichever shell generation is live.
    session.setCallbacks({
      .onStatus = reportStatus,
      .onFailure =
        [weak](ao::Error const& error)
      {
        if (auto self = weak.get(); self)
        {
          self->updateStatus(ao::winui::formatResource("winui_error", error.message));
        }
      },
    });

    _trackPropertiesPtr =
      std::make_unique<ao::winui::TrackPropertiesPresenter>(ao::winui::TrackPropertiesPresenterConfig{
        .xamlRoot = currentRoot,
        .asyncRuntime = runtime.async(),
        .library = runtime.library(),
        .workspace = runtime.workspace(),
        .completion = runtime.completion(),
        .textCatalog = session.textCatalog(),
        .reportStatus = reportStatus,
      });
    _listAuthoringCoordinatorPtr =
      std::make_unique<ao::winui::ListAuthoringCoordinator>(ao::winui::ListAuthoringCoordinatorConfig{
        .xamlRoot = currentRoot,
        .asyncRuntime = runtime.async(),
        .library = runtime.library(),
        .views = runtime.views(),
        .sources = runtime.sources(),
        .notifications = runtime.notifications(),
        .trackList = *_trackListPtr,
        .presentationCatalog = session.presentationCatalog(),
        .listPresentations = session.listPresentations(),
        .textOrderingPolicy = runtime.textOrderingPolicy(),
        .textCatalog = session.textCatalog(),
        .reportStatus = reportStatus,
      });
    _libraryTransferCoordinatorPtr =
      std::make_unique<ao::winui::LibraryTransferCoordinator>(ao::winui::LibraryTransferCoordinatorConfig{
        .xamlRoot = currentRoot,
        .windowId = AppWindow().Id(),
        .asyncRuntime = runtime.async(),
        .jobs = runtime.library().jobs(),
        .notifications = runtime.notifications(),
        .textCatalog = session.textCatalog(),
        .reportStatus = reportStatus,
      });

    if (auto themeRes = _themePtr->reload(); themeRes)
    {
      applyTheme(*themeRes);
    }
    else if (themeRes.error().code != ao::Error::Code::NotFound)
    {
      // The window keeps following Windows appearance, which is what the
      // user sees; the feed and the status line say why their theme is not in force.
      auto message = ao::i18n::requiredFormat(
        session.textCatalog(), ao::i18n::MessageId::WinUiThemeRejected, {{"detail", themeRes.error().message}});
      runtime.notifications().post(
        ao::rt::NotificationSeverity::Warning, message, ao::rt::NotificationLifetime::history());
      session.reportStartupWarning(std::move(message));
    }

    // Built before the first resolved policy, which is what asks it for a shell.
    createShellBuilder();
    updateSoulWindowActivity();
    ao::winui::restoreWindowPlacement(*this, session.settings().window);
    tryApplyShellState(RootGrid().ActualWidth());

    // The shell built above is the first to show a status, so the startup
    // warnings reach the status line only here.
    auto startupStatus = session.startupStatus();
    updateStatus(startupStatus.empty() ? ao::winui::resourceString("winui_library_ready") : std::move(startupStatus));
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

  void MainWindow::bindPlayback()
  {
    if (_session == nullptr)
    {
      return;
    }

    unbindPlayback();
    auto& runtime = _session->runtime();
    auto& playback = runtime.playback();
    auto& actions = _session->playbackActions();

    if (_shellOutputDevicePtr)
    {
      _shellOutputDevicePtr->bind(playback);
    }

    _playback.bindTransport(playback, actions, _session->textCatalog());

    // System media controls are optional; a failure here must not disable the shell's transport commands.
    try
    {
      _playback.bindSystemMediaControls(
        ao::winui::nativeWindowHandle(*this), playback, actions, runtime.resourceBytes(), runtime.async());
    }
    catch (hresult_error const& error)
    {
      updateStatus(ao::winui::formatResource("winui_smtc_unavailable", to_string(error.message())));
    }
  }

  void MainWindow::unbindPlayback() noexcept
  {
    _playback.unbind();

    if (_shellOutputDevicePtr)
    {
      _shellOutputDevicePtr->unbind();
    }
  }

  void MainWindow::updateSoulWindowActivity()
  {
    // The frame supplies visibility; the live shell generation owns the souls it applies to.
    if (auto const window = AppWindow(); _shellBuilderPtr)
    {
      _shellBuilderPtr->applyWindowActivity(window.IsVisible(), ao::winui::isWindowMinimized(window));
    }
  }

  void MainWindow::playPause()
  {
    _playback.playPause();
  }

  void MainWindow::stopPlayback()
  {
    _playback.stop();
  }

  void MainWindow::showOutputDeviceSelector(Microsoft::UI::Xaml::FrameworkElement const& anchor)
  {
    if (_shellOutputDevicePtr)
    {
      _shellOutputDevicePtr->showAt(anchor);
    }
  }

  void MainWindow::showFullscreenSoul()
  {
    if (_fullscreenSoulWindowPtr)
    {
      _fullscreenSoulWindowPtr->show();
    }
  }

  bool MainWindow::isModalWorkflowActive() const noexcept
  {
    return (_listAuthoringCoordinatorPtr && _listAuthoringCoordinatorPtr->isDialogActive()) ||
           (_libraryTransferCoordinatorPtr && _libraryTransferCoordinatorPtr->isActive()) ||
           (_trackPropertiesPtr && _trackPropertiesPtr->isActive());
  }

  void MainWindow::presentTrackProperties()
  {
    if (_sessionPhase != SessionPhase::Retired && _trackPropertiesPtr && _trackListPtr && !isModalWorkflowActive())
    {
      _trackPropertiesPtr->presentSelection(_trackListPtr->selection());
    }
  }

  void MainWindow::navigateHistory(bool const forward)
  {
    if (_session == nullptr || isModalWorkflowActive())
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

  void MainWindow::handleNavigateBackInvoked(
    Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
    Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
  {
    navigateHistory(false);
    args.Handled(true);
  }

  void MainWindow::handleNavigateForwardInvoked(
    Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
    Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
  {
    navigateHistory(true);
    args.Handled(true);
  }

  void MainWindow::handleRootPointerPressed(Windows::Foundation::IInspectable const& /*sender*/,
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
        // the window itself started while its own consumers are still alive.
        .trackList = *_trackListPtr,
        .resourceBytes = _session->runtime().resourceBytes(),
        .theme = *_themePtr,
        .activeTheme = [this] { return _optThemeOverride; },
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
        },
        .showOutputDeviceSelector =
          [weak](Microsoft::UI::Xaml::FrameworkElement const& anchor)
        {
          if (auto self = weak.get(); self)
          {
            self->showOutputDeviceSelector(anchor);
          }
        },
        .listCommands = {
          .createList =
            [weak](ao::ListId const parentListId, std::string expression)
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->isModalWorkflowActive())
            {
              self->_listAuthoringCoordinatorPtr->createList(parentListId, std::move(expression));
            }
          },
          .editList =
            [weak](ao::ListId const listId)
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->isModalWorkflowActive())
            {
              self->_listAuthoringCoordinatorPtr->editList(listId);
            }
          },
          .deleteList =
            [weak](ao::ListId const listId, bool const includeDescendants)
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->isModalWorkflowActive())
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
                self && self->_listAuthoringCoordinatorPtr && !self->isModalWorkflowActive())
            {
              self->_listAuthoringCoordinatorPtr->editMembership(listId, add);
            }
          },
          .orderCapabilities =
            [weak]
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->isModalWorkflowActive())
            {
              return self->_listAuthoringCoordinatorPtr->orderCapabilities();
            }

            return ao::uimodel::ListOrderCapabilityState{};
          },
          .applyOrder =
            [weak](ao::winui::ListOrderCommand const commandValue)
          {
            if (auto self = weak.get();
                self && self->_listAuthoringCoordinatorPtr && !self->isModalWorkflowActive())
            {
              self->_listAuthoringCoordinatorPtr->applyOrder(commandValue);
            }
          },
        },
        .editCredits = [weak](auto ids, auto scope)
        {
          if (auto self = weak.get(); self && self->_sessionPhase != SessionPhase::Retired && self->_trackPropertiesPtr && !self->isModalWorkflowActive())
          {
            self->_trackPropertiesPtr->editCredits(std::move(ids), scope);
          }
        },
      });
  }

  void MainWindow::handleRootSizeChanged(Windows::Foundation::IInspectable const& /*sender*/,
                                         Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
  {
    tryApplyShellState(args.NewSize().Width);
  }

  void MainWindow::showSystemMenu()
  {
    ao::winui::showNativeSystemMenu(*this);
  }

  bool MainWindow::tryApplyShellState(double const width, std::optional<ao::winui::ShellMode> const optCandidateMode)
  {
    // The window is sized before it has a session to build a shell from, so the
    // first resolved policy is the one `initialize` asks for.
    if (!_shellBuilderPtr)
    {
      return false;
    }

    auto const mode =
      optCandidateMode.value_or(_session != nullptr ? _session->settings().shellMode : ao::winui::ShellMode::Modern);
    auto appliedRes = _shellBuilderPtr->applyShellState(mode, width, _optInspectorRequest);

    if (!appliedRes)
    {
      // The generation that was already live stays live, which is the whole
      // point of building the candidate before publishing it.
      updateStatus(ao::winui::formatResource("winui_shell_layout_failed", appliedRes.error().message));
      return false;
    }

    auto const& state = *appliedRes;

    if (state.mode != mode)
    {
      return false;
    }

    ao::winui::runOptionalWinRt(
      "applying the WinUI title bar",
      [this, &state]
      {
        ExtendsContentIntoTitleBar(state.integratedTitleBar);
        SetTitleBar(state.integratedTitleBar ? _shellBuilderPtr->titleBar() : Microsoft::UI::Xaml::UIElement{nullptr});
      });
    return true;
  }

  void MainWindow::saveWindowState() noexcept
  {
    try
    {
      if (_session == nullptr)
      {
        return;
      }

      if (auto const optPlacement = ao::winui::readWindowPlacement(*this); optPlacement)
      {
        ao::winui::rememberDesktopWindowPlacement(_session->settings(), *optPlacement);
      }

      if (auto const savedRes = _session->saveSettings(); !savedRes)
      {
        updateStatus(ao::winui::formatResource("winui_save_settings_failed", savedRes.error().message));
      }
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "WinUI window-state checkpoint");
    }
  }

  winrt::fire_and_forget MainWindow::pickLibrary()
  {
    auto lifetime = get_strong();
    auto exceptionPtr = std::exception_ptr{};

    try
    {
      auto picker = Microsoft::Windows::Storage::Pickers::FolderPicker{AppWindow().Id()};
      picker.SuggestedStartLocation(Microsoft::Windows::Storage::Pickers::PickerLocationId::MusicLibrary);
      picker.CommitButtonText(ao::winui::resourceHstring(L"winui_open_library_picker"));

      if (auto result = co_await picker.PickSingleFolderAsync();
          result && !result.Path().empty() && _session != nullptr &&
          (_sessionPhase == SessionPhase::Prepared || _sessionPhase == SessionPhase::Active) && _requestRestart)
      {
        // The callback only queues an App-owned restart. Its dispatcher turn
        // runs after this coroutine returns and releases its strong window ref.
        if (auto const requestedRes = _requestRestart(std::filesystem::path{result.Path().c_str()}); !requestedRes)
        {
          updateStatus(ao::winui::formatResource("winui_error", requestedRes.error().message));
        }
      }
    }
    catch (hresult_error const& error)
    {
      updateStatus(ao::winui::formatResource("winui_folder_picker_failed", to_string(error.message())));
    }
    catch (...)
    {
      exceptionPtr = std::current_exception();
    }

    if (exceptionPtr && !ao::async::isOperationCancelled(exceptionPtr))
    {
      AO_FATAL_EXCEPTION(exceptionPtr, "WinUI library folder picker");
    }
  }

  void MainWindow::rescanLibrary()
  {
    if (_session != nullptr)
    {
      _session->rescan();
    }
  }

  void MainWindow::importLibrary()
  {
    if (_libraryTransferCoordinatorPtr && !isModalWorkflowActive())
    {
      _libraryTransferCoordinatorPtr->importLibrary();
    }
  }

  void MainWindow::exportLibrary()
  {
    if (_libraryTransferCoordinatorPtr && !isModalWorkflowActive())
    {
      _libraryTransferCoordinatorPtr->exportLibrary();
    }
  }

  void MainWindow::toggleShellMode()
  {
    if (_session == nullptr)
    {
      return;
    }

    auto& settings = _session->settings();
    auto const candidateMode =
      settings.shellMode == ao::winui::ShellMode::Modern ? ao::winui::ShellMode::Classic : ao::winui::ShellMode::Modern;

    if (tryApplyShellState(RootGrid().ActualWidth(), candidateMode))
    {
      settings.shellMode = candidateMode;
      saveWindowState();
    }
  }

  void MainWindow::reloadTheme()
  {
    if (_themePtr == nullptr)
    {
      return;
    }

    auto reloadedRes = _themePtr->reload();

    if (!reloadedRes)
    {
      if (reloadedRes.error().code == ao::Error::Code::NotFound)
      {
        applySystemTheme();
        updateStatus(ao::winui::resourceString("winui_theme_override_removed"));
        return;
      }

      updateStatus(ao::winui::formatResource("winui_theme_reload_failed", reloadedRes.error().message));
      return;
    }

    applyTheme(*reloadedRes);
    updateStatus(ao::winui::formatResource("winui_theme_reloaded", ao::utility::pathToUtf8(_themePtr->path())));
  }

  /**
   * @brief Rebuild the document-built shell against the theme now in force.
   *
   * A preset's `surface` slots are painted from the override at construction,
   * so unlike a `ThemeResource` they do not re-resolve on their own. The whole
   * generation is rebuilt rather than repainted: that is the one path a preset
   * change is already known to survive.
   */
  void MainWindow::rebuildForTheme()
  {
    if (!_shellBuilderPtr)
    {
      return;
    }

    if (auto rebuiltRes = _shellBuilderPtr->rebuild(); !rebuiltRes)
    {
      updateStatus(ao::winui::formatResource("winui_shell_layout_failed", rebuiltRes.error().message));
    }
  }

  void MainWindow::applyTheme(ao::winui::Theme const& theme)
  {
    _optThemeOverride = theme;
    ao::winui::applyFrameTheme(RootGrid(), theme);
    rebuildForTheme();
  }

  void MainWindow::applySystemTheme()
  {
    _optThemeOverride.reset();
    ao::winui::applySystemFrameTheme(RootGrid());
    rebuildForTheme();
  }

  void MainWindow::updateStatus(std::string const& status)
  {
    // The shell retains the message, so a generation built later still shows it.
    if (_shellBuilderPtr)
    {
      _shellBuilderPtr->reportStatus(status);
    }
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
    _groupCoverPresentersPtr.reset();

    // Cancel authoring and close its transient native tree before releasing
    // either the shell selection that opened it or the runtime it submits to.
    _trackPropertiesPtr.reset();

    // Generation callbacks and all generation-owned runtime borrowers go away
    // before the window's own consumers are released. Releasing the owner is
    // the retirement: each destructor runs its own quiescence, so the order of
    // these resets is the whole contract.
    _shellBuilderPtr.reset();
    _libraryTransferCoordinatorPtr.reset();
    _listAuthoringCoordinatorPtr.reset();

    // Close the secondary window and detach the frame's playback adapters
    // while the borrowed playback service is still live.
    _fullscreenSoulWindowPtr.reset();
    unbindPlayback();

    // The window's own retirement, run as named steps rather than left to
    // member order: status publication and reveal routing stop before the
    // models they publish into are released, and the models go in the order
    // their borrowers were released.
    if (_session != nullptr)
    {
      _session->setCallbacks({});
    }

    _revealTrackSub.reset();
    _themePtr.reset();
    _trackListPtr.reset();

    _shellOutputDevicePtr.reset();
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

namespace ao::winui
{
  namespace
  {
    using Window = winrt::Microsoft::UI::Xaml::Window;
    using MainWindow = winrt::Aobus::implementation::MainWindow;

    MainWindow& mainWindowImplementation(Window const& window)
    {
      // Drop the temporary query reference before calling any frame command.
      return *winrt::get_self<MainWindow>(window.as<winrt::Aobus::MainWindow>());
    }
  } // namespace

  void LibraryWindowSession::createAndInitializeMainWindow(RestartRequest&& requestRestart)
  {
    // Store the window first so the session can retire it if initialization fails.
    _window = winrt::make<MainWindow>();
    auto& frame = mainWindowImplementation(_window);
    frame.initialize(*_sessionPtr, std::move(requestRestart));
  }

  Result<> LibraryWindowSession::activateMainWindowAdapters()
  {
    return mainWindowImplementation(_window).activate();
  }

  Result<> LibraryWindowSession::prepareMainWindowForLibraryRestart()
  {
    return mainWindowImplementation(_window).prepareLibraryRestart();
  }

  void LibraryWindowSession::retireMainWindow(Window const& window)
  {
    mainWindowImplementation(window).retire();
  }
} // namespace ao::winui
