// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/LibraryWindowSession.h"

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "pch.h"
#include <ao/Error.h>
#include <ao/rt/Log.h>
#include <ao/winui/app/StartupOptions.h>

#include <winrt/Microsoft.UI.Xaml.h>

#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string_view>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using Window = winrt::Microsoft::UI::Xaml::Window;
    using MainWindow = winrt::Aobus::implementation::MainWindow;

    Error exceptionError(std::string_view const operation, std::exception const& error)
    {
      return Error{.code = Error::Code::InitFailed, .message = std::format("{}: {}", operation, error.what())};
    }

    Error unknownExceptionError(std::string_view const operation)
    {
      return Error{.code = Error::Code::InitFailed, .message = std::format("{}: unknown exception", operation)};
    }
  } // namespace

  LibraryWindowSession::LibraryWindowSession(std::filesystem::path stateRoot,
                                             winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher)
    : _stateRoot{std::move(stateRoot)}, _dispatcher{std::move(dispatcher)}
  {
  }

  LibraryWindowSession::~LibraryWindowSession()
  {
    retire();
  }

  void LibraryWindowSession::installClosedHandler(Window const& window)
  {
    _windowClosedRevoker =
      window.Closed(winrt::auto_revoke,
                    [this](winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::WindowEventArgs const&) noexcept { handleClosed(); });
  }

  Result<> LibraryWindowSession::start(StartupOptions options, RestartRequest requestRestart, ClosedCallback onClosed)
  {
    if (_started)
    {
      return makeError(Error::Code::InvalidState, "The WinUI library window session has already been started");
    }

    _started = true;
    _onClosed = std::move(onClosed);

    try
    {
      _sessionPtr = std::make_unique<LibrarySession>(_stateRoot, _dispatcher, std::move(options));
      _window = winrt::make<MainWindow>();
      auto* implementation = winrt::get_self<MainWindow>(_window.as<winrt::Aobus::MainWindow>());
      implementation->initialize(*_sessionPtr, std::move(requestRestart));
      installClosedHandler(_window);
      _window.Activate();

      if (!_window || !_sessionPtr)
      {
        retire();
        return makeError(Error::Code::InvalidState, "The WinUI library window closed during startup");
      }

      implementation = winrt::get_self<MainWindow>(_window.as<winrt::Aobus::MainWindow>());

      if (auto activated = implementation->activate(); !activated)
      {
        retire();
        return activated;
      }

      // The explicit successor root becomes durable only after both the native
      // window and its process-wide adapters are active. A failed save leaves
      // the usable process live and later settings saves retry the in-memory root.
      if (auto committed = _sessionPtr->commitSelectedRoot(); !committed)
      {
        APP_LOG_WARN("LibraryWindowSession: failed to persist the selected library: {}", committed.error().message);
      }

      if (_sessionPtr->scanAfterOpen())
      {
        _sessionPtr->rescan();
      }

      return {};
    }
    catch (std::exception const& error)
    {
      retire();
      return std::unexpected{exceptionError("Failed to start the WinUI library window session", error)};
    }
    catch (...)
    {
      retire();
      return std::unexpected{unknownExceptionError("Failed to start the WinUI library window session")};
    }
  }

  std::filesystem::path const& LibraryWindowSession::musicRoot() const noexcept
  {
    return _sessionPtr->musicRoot();
  }

  bool LibraryWindowSession::active() const noexcept
  {
    return _window != nullptr && _sessionPtr != nullptr;
  }

  void LibraryWindowSession::retireWindow() noexcept
  {
    if (!_window)
    {
      return;
    }

    auto window = std::move(_window);
    _windowClosedRevoker.revoke();

    try
    {
      auto* const implementation = winrt::get_self<MainWindow>(window.as<winrt::Aobus::MainWindow>());
      implementation->retire();
    }
    // NOLINTNEXTLINE(bugprone-empty-catch): Closing a projected window is best-effort during teardown.
    catch (...)
    {
      // MainWindow retirement is progressive and no-throw. Keep releasing the
      // native owner if an unexpected C++/WinRT boundary still fails.
    }

    try
    {
      window.Close();
    }
    // NOLINTNEXTLINE(bugprone-empty-catch): The closed event cannot make retirement fail.
    catch (...)
    {
      // Releasing the projected owner below remains mandatory.
    }

    window = nullptr;
  }

  void LibraryWindowSession::releaseSession() noexcept
  {
    // LibrarySession is a plain owner behind unique_ptr, so its destructor
    // already runs the quiescence in order. Naming it here as well would be a
    // second caller that has to be kept in step with the first.
    _sessionPtr.reset();
  }

  void LibraryWindowSession::retire() noexcept
  {
    _onClosed = {};
    retireWindow();
    releaseSession();
  }

  void LibraryWindowSession::handleClosed() noexcept
  {
    if (!_window)
    {
      return;
    }

    _windowClosedRevoker.revoke();
    auto closedWindow = std::move(_window);

    try
    {
      auto* const implementation = winrt::get_self<MainWindow>(closedWindow.as<winrt::Aobus::MainWindow>());
      implementation->retire();
    }
    // NOLINTNEXTLINE(bugprone-empty-catch): A closed window must not block release of the remaining session owners.
    catch (...)
    {
    }

    closedWindow = nullptr;
    releaseSession();

    if (auto onClosed = std::move(_onClosed); onClosed)
    {
      onClosed();
    }
  }
} // namespace ao::winui
