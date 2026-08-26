// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "App.xaml.h"

#include "app/LibraryWindowSession.h"
#include "platform/ProcessLauncher.h"
#include "platform/StringResources.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/i18n/IcuCompletionAliases.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>
#include <ao/utility/PlatformDirectories.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/app/DestructiveLibraryRestart.h>

#include <windows.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace winrt::Aobus::implementation
{
  namespace
  {
    constexpr auto kErrorDialogFlags = MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST;

    void showStartupFailure(std::string_view detail) noexcept;

    /**
     * @brief Last resort for an exception that escaped a no-throw boundary.
     *
     * The floor exists so that such a bug leaves a diagnostic rather than a
     * bare crash dialog. It is not a licence to skip cleanup: every teardown
     * path still has to release its own owners, and reaching here at all means
     * one of them was written wrong.
     */
    [[noreturn]] void reportTerminate() noexcept
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "WinUI terminate handler");
    }

    ao::Result<std::filesystem::path> stateRoot()
    {
      return ao::utility::applicationConfigDirectory();
    }

    void showStartupFailure(std::string_view const detail) noexcept
    {
      try
      {
        auto const message = ao::winui::formatResource("winui_startup_failure", detail);
        auto const messageText = to_hstring(message);
        auto const title = ao::winui::resourceHstring(L"AppTitleValue");
        ::MessageBoxW(nullptr, messageText.c_str(), title.c_str(), kErrorDialogFlags);
      }
      catch (...)
      {
        AO_AUDITED_CATCH(DiagnosticFallback);
        ::MessageBoxW(nullptr, L"Aobus could not start.", L"Aobus", kErrorDialogFlags);
      }
    }
  } // namespace

  App::App()
  {
    std::ignore = std::set_terminate(&reportTerminate);

    auto catalogRes = ao::i18n::MessageCatalog::createForSystemLocale();

    if (!catalogRes)
    {
      AO_FATAL("Could not initialize WinUI localization: {}", catalogRes.error().message);
    }

    _messageCatalogPtr = std::make_unique<ao::i18n::MessageCatalog>(std::move(*catalogRes));
    _presentationTextCatalogPtr = std::make_unique<ao::uimodel::PresentationTextCatalog>(*_messageCatalogPtr);
    auto textOrderingPolicyRes = ao::i18n::createIcuTextOrderingPolicy(_messageCatalogPtr->requestedLocale());

    if (!textOrderingPolicyRes)
    {
      AO_FATAL("Could not initialize WinUI text ordering: {}", textOrderingPolicyRes.error().message);
    }

    _textOrderingPolicyPtr = std::move(*textOrderingPolicyRes);
    _completionAliasPolicyPtr = ao::i18n::createIcuCompletionAliasPolicy();

    auto resourceLanguageRes = ao::winui::configureResourceLanguage(_messageCatalogPtr->requestedLocale());

    if (!resourceLanguageRes)
    {
      AO_FATAL("Could not bind WinUI resources to the application locale: {}", resourceLanguageRes.error().message);
    }

    InitializeComponent();
  }

  App::~App()
  {
    _windowSessionPtr.reset();

    try
    {
      ao::winui::resetResourceLanguage();
      _completionAliasPolicyPtr.reset();
      _textOrderingPolicyPtr.reset();
      _presentationTextCatalogPtr.reset();
      _messageCatalogPtr.reset();
    }
    catch (...)
    {
      AO_AUDITED_CATCH(SafeCleanup);
      ::OutputDebugStringA("Aobus could not release WinUI localization cleanly.\n");
    }

    try
    {
      ao::rt::Log::shutdown();
    }
    catch (...)
    {
      AO_AUDITED_CATCH(SafeCleanup);
      ::OutputDebugStringA("Aobus could not shut down WinUI logging cleanly.\n");
    }
  }

  void App::exitApplication() noexcept
  {
    _dispatcher = nullptr;

    try
    {
      Microsoft::UI::Xaml::Application::Current().Exit();
    }
    catch (...)
    {
      AO_AUDITED_CATCH(PlatformFallback);
      ::PostQuitMessage(1);
    }
  }

  void App::reportRestartLaunchFailure(ao::Error const& error) noexcept
  {
    showStartupFailure(error.message);
    ao::winui::logWinUiCritical("WinUI successor launch failed", error.message);
  }

  ao::Result<> App::requestLibraryRestart(std::filesystem::path root)
  {
    if (_processPhase != ProcessPhase::Running || !_windowSessionPtr || !_windowSessionPtr->active())
    {
      return ao::makeError(ao::Error::Code::ResourceBusy, "A WinUI process transition is already in progress");
    }

    auto switchPlanRes = ao::desktop::planLibrarySwitch(_windowSessionPtr->musicRoot(), std::move(root), false);

    if (!switchPlanRes)
    {
      return std::unexpected{switchPlanRes.error()};
    }

    if (switchPlanRes->disposition == ao::desktop::LibrarySwitchDisposition::ReuseActive)
    {
      return {};
    }

    if (!_dispatcher)
    {
      return ao::makeError(ao::Error::Code::InvalidState, "The WinUI dispatcher is unavailable");
    }

    _processPhase = ProcessPhase::RestartQueued;
    auto const weak = get_weak();

    try
    {
      auto const queued = _dispatcher.TryEnqueue(
        [weak, request = std::move(switchPlanRes->request)] mutable
        {
          if (auto self = weak.get(); self)
          {
            self->performLibraryRestart(std::move(request));
          }
        });

      if (!queued)
      {
        _processPhase = ProcessPhase::Running;
        return ao::makeError(
          ao::Error::Code::ResourceBusy, "The WinUI dispatcher rejected the library restart request");
      }
    }
    catch (winrt::hresult_error const& error)
    {
      _processPhase = ProcessPhase::Running;
      return ao::makeError(ao::Error::Code::InitFailed,
                           std::format("Failed to queue the library restart: {}", winrt::to_string(error.message())));
    }

    return {};
  }

  void App::performLibraryRestart(ao::desktop::LibrarySwitchRequest request) noexcept
  {
    if (_processPhase != ProcessPhase::RestartQueued)
    {
      return;
    }

    _processPhase = ProcessPhase::Exiting;

    // Releasing the owner releases its window first: LibraryWindowSession
    // declares its session before its window, so reverse member destruction
    // fixes that order without this call site restating it.
    auto const outcome = ao::winui::executeDestructiveLibraryRestart({
      .prepareActiveGraph = [this] { return _windowSessionPtr->prepareLibraryRestart(); },
      .releaseActiveGraph = [this] { _windowSessionPtr.reset(); },
      .launchSuccessor = [request = std::move(request)] { return ao::winui::launchLibraryProcess(request); },
      .reportPreparationFailure = [](ao::Error const& error) noexcept
      { ao::winui::logWinUiCritical("WinUI library restart preparation failed", error.message); },
      .reportLaunchFailure = [this](ao::Error const& error) noexcept { reportRestartLaunchFailure(error); },
      .exitProcess = [this] noexcept { exitApplication(); },
    });

    if (outcome == ao::winui::DestructiveLibraryRestartOutcome::PreparationFailed)
    {
      _processPhase = ProcessPhase::Running;
    }
  }

  void App::handleWindowClosed() noexcept
  {
    // Once a restart is queued, the queued dispatcher turn owns successor
    // launch even if the user closes the old HWND before that turn runs.
    if (_processPhase == ProcessPhase::RestartQueued)
    {
      return;
    }

    _processPhase = ProcessPhase::Exiting;
    exitApplication();
  }

  void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& /*args*/)
  {
    auto const failLaunch = [this](std::string_view const detail) noexcept
    {
      _processPhase = ProcessPhase::Exiting;
      showStartupFailure(detail);
      ao::winui::logWinUiCritical("WinUI startup failed", detail);

      if (_windowSessionPtr)
      {
        _windowSessionPtr->retire();
        _windowSessionPtr.reset();
      }

      exitApplication();
    };

    try
    {
      _dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
      auto appStateRootRes = stateRoot();

      if (!appStateRootRes)
      {
        failLaunch(appStateRootRes.error().message);
        return;
      }

      auto const appStateRoot = std::move(*appStateRootRes);
      ao::rt::Log::initialize(ao::rt::LogLevel::Info, appStateRoot / "logs", ao::rt::LogConsoleMode::Disabled);
      auto startupRequestRes = ao::winui::readLibrarySuccessorRequest();

      if (!startupRequestRes)
      {
        failLaunch(startupRequestRes.error().message);
        return;
      }

      _windowSessionPtr = std::make_unique<ao::winui::LibraryWindowSession>(
        appStateRoot, _dispatcher, *_presentationTextCatalogPtr, *_textOrderingPolicyPtr, *_completionAliasPolicyPtr);
      auto const weak = get_weak();
      auto startedRes = _windowSessionPtr->start(
        std::move(*startupRequestRes),
        [weak](std::filesystem::path root) -> ao::Result<>
        {
          if (auto self = weak.get(); self)
          {
            return self->requestLibraryRestart(std::move(root));
          }

          return ao::makeError(ao::Error::Code::InvalidState, "The WinUI application is shutting down");
        },
        [weak] noexcept
        {
          if (auto self = weak.get(); self)
          {
            self->handleWindowClosed();
          }
        });

      if (!startedRes)
      {
        failLaunch(startedRes.error().message);
        return;
      }

      if (_processPhase != ProcessPhase::Exiting)
      {
        _processPhase = ProcessPhase::Running;
      }
    }
    catch (winrt::hresult_error const& error)
    {
      try
      {
        auto const detail = ao::winui::formatResource("StartupHresultDetailFormat",
                                                      winrt::to_string(error.message()),
                                                      static_cast<std::uint32_t>(error.code().value));
        failLaunch(detail);
      }
      catch (...)
      {
        AO_AUDITED_CATCH(DiagnosticFallback);
        failLaunch("Windows reported a startup error.");
      }
    }
    catch (...)
    {
      auto exceptionPtr = std::current_exception();
      _processPhase = ProcessPhase::Exiting;

      if (_windowSessionPtr)
      {
        _windowSessionPtr->retire();
        _windowSessionPtr.reset();
      }

      AO_FATAL_EXCEPTION(std::move(exceptionPtr), "WinUI launch root");
    }
  }
} // namespace winrt::Aobus::implementation
