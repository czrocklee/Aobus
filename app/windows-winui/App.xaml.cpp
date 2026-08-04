// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "App.xaml.h"

#include "app/LibraryWindowSession.h"
#include "platform/ProcessLauncher.h"
#include "platform/StringResources.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/rt/Log.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/app/DestructiveLibraryRestart.h>

#include <windows.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace winrt::Aobus::implementation
{
  namespace
  {
    constexpr std::size_t kEnvironmentBufferLength = 32'768;

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
      if (auto const exceptionPtr = std::current_exception(); exceptionPtr)
      {
        try
        {
          std::rethrow_exception(exceptionPtr);
        }
        catch (std::exception const& error)
        {
          ao::winui::logWinUiCritical("WinUI terminating on an escaped exception", error.what());
        }
        catch (...)
        {
          ao::winui::logWinUiCritical("WinUI terminating", "escaped unknown exception");
        }
      }
      else
      {
        ao::winui::logWinUiCritical("WinUI terminating", "no active exception");
      }

      // Quit without unwinding: whatever invariant broke, running more
      // destructors over it is how a diagnosable fault becomes a corrupt one.
      std::_Exit(EXIT_FAILURE);
    }

    std::filesystem::path stateRoot()
    {
      auto buffer = std::array<wchar_t, kEnvironmentBufferLength>{};
      auto const length = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));

      if (length == 0 || length >= buffer.size())
      {
        ao::throwException<ao::Exception>("LOCALAPPDATA is unavailable");
      }

      return std::filesystem::path{std::wstring_view{buffer.data(), length}} / "Aobus";
    }

    void showStartupFailure(std::string_view const detail) noexcept
    {
      try
      {
        auto const message = ao::winui::formatResource("StartupFailureFormat", detail);
        auto const messageText = to_hstring(message);
        auto const title = ao::winui::resourceHstring(L"AppTitleValue");
        ::MessageBoxW(nullptr, messageText.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
      }
      catch (...)
      {
        ::MessageBoxW(nullptr, L"Aobus could not start.", L"Aobus", MB_OK | MB_ICONERROR);
      }
    }

    bool sameDirectory(std::filesystem::path const& left, std::filesystem::path const& right) noexcept
    {
      auto error = std::error_code{};
      auto const equivalent = std::filesystem::equivalent(left, right, error);
      return !error ? equivalent : left.lexically_normal() == right.lexically_normal();
    }

    ao::Result<std::filesystem::path> normalizeRestartRoot(std::filesystem::path root)
    {
      if (root.empty())
      {
        return ao::makeError(ao::Error::Code::InvalidInput, "The selected library path is empty");
      }

      auto error = std::error_code{};
      auto absolute = std::filesystem::absolute(std::move(root), error);

      if (error)
      {
        return ao::makeError(
          ao::Error::Code::IoError, std::format("Failed to resolve the selected library path: {}", error.message()));
      }

      return absolute.lexically_normal();
    }
  } // namespace

  App::App()
  {
    std::ignore = std::set_terminate(&reportTerminate);
    InitializeComponent();
  }

  App::~App()
  {
    _windowSessionPtr.reset();

    try
    {
      ao::rt::Log::shutdown();
    }
    catch (...)
    {
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
      ::PostQuitMessage(1);
    }
  }

  void App::reportRestartLaunchFailure(ao::Error const& error) noexcept
  {
    ao::winui::logWinUiCritical("WinUI successor launch failed", error.message);

    showStartupFailure(error.message);
  }

  ao::Result<> App::requestLibraryRestart(std::filesystem::path root)
  {
    if (_processPhase != ProcessPhase::Running || !_windowSessionPtr || !_windowSessionPtr->active())
    {
      return ao::makeError(ao::Error::Code::ResourceBusy, "A WinUI process transition is already in progress");
    }

    auto normalized = normalizeRestartRoot(std::move(root));

    if (!normalized)
    {
      return std::unexpected{normalized.error()};
    }

    if (sameDirectory(*normalized, _windowSessionPtr->musicRoot()))
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
        [weak, root = std::move(*normalized)] mutable
        {
          if (auto self = weak.get(); self)
          {
            self->performLibraryRestart(std::move(root));
          }
        });

      if (!queued)
      {
        _processPhase = ProcessPhase::Running;
        return ao::makeError(
          ao::Error::Code::ResourceBusy, "The WinUI dispatcher rejected the library restart request");
      }
    }
    catch (std::exception const& error)
    {
      _processPhase = ProcessPhase::Running;
      return ao::makeError(
        ao::Error::Code::InitFailed, std::format("Failed to queue the library restart: {}", error.what()));
    }
    catch (...)
    {
      _processPhase = ProcessPhase::Running;
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to queue the library restart: unknown exception");
    }

    return {};
  }

  void App::performLibraryRestart(std::filesystem::path root) noexcept
  {
    if (_processPhase != ProcessPhase::RestartQueued)
    {
      return;
    }

    _processPhase = ProcessPhase::Exiting;

    // Releasing the owner releases its window first: LibraryWindowSession
    // declares its session before its window, so reverse member destruction
    // fixes that order without this call site restating it.
    std::ignore = ao::winui::executeDestructiveLibraryRestart({
      .releaseActiveGraph = [this] { _windowSessionPtr.reset(); },
      .launchSuccessor = [root = std::move(root)] { return ao::winui::launchLibraryProcess(root); },
      .reportLaunchFailure = [this](ao::Error const& error) noexcept { reportRestartLaunchFailure(error); },
      .exitProcess = [this] noexcept { exitApplication(); },
    });
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
    auto const failLaunch = [this](std::string detail) noexcept
    {
      _processPhase = ProcessPhase::Exiting;
      ao::winui::logWinUiCritical("WinUI startup failed", detail);

      if (_windowSessionPtr)
      {
        _windowSessionPtr->retire();
        _windowSessionPtr.reset();
      }

      showStartupFailure(detail);
      exitApplication();
    };

    try
    {
      _dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
      auto const appStateRoot = stateRoot();
      ao::rt::Log::initialize(ao::rt::LogLevel::Info, appStateRoot / "logs", ao::rt::LogConsoleMode::Disabled);
      auto startupOptions = ao::winui::readStartupOptions();

      if (!startupOptions)
      {
        ao::throwException<ao::Exception>(startupOptions.error().message);
      }

      _windowSessionPtr = std::make_unique<ao::winui::LibraryWindowSession>(appStateRoot, _dispatcher);
      auto const weak = get_weak();
      auto started = _windowSessionPtr->start(
        std::move(*startupOptions),
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

      if (!started)
      {
        ao::throwException<ao::Exception>(started.error().message);
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
        failLaunch("Windows reported a startup error.");
      }
    }
    catch (std::exception const& error)
    {
      failLaunch(error.what());
    }
    catch (...)
    {
      failLaunch(ao::winui::resourceString("StartupFailureUnknown"));
    }
  }
} // namespace winrt::Aobus::implementation
