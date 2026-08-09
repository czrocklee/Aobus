// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/LibraryWindowSession.h"

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "pch.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/rt/Log.h>
#include <ao/winui/app/StartupOptions.h>

#include <winrt/Microsoft.UI.Xaml.h>

#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using Window = winrt::Microsoft::UI::Xaml::Window;
    using MainWindow = winrt::Aobus::implementation::MainWindow;

    Error hresultError(std::string_view const operation, winrt::hresult_error const& error)
    {
      return Error{.code = Error::Code::InitFailed,
                   .message = std::format("{}: {}", operation, winrt::to_string(error.message()))};
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
      auto sessionRes = LibrarySession::create(_stateRoot, _dispatcher, std::move(options));

      if (!sessionRes)
      {
        retire();
        return std::unexpected{sessionRes.error()};
      }

      _sessionPtr = std::move(*sessionRes);
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

      if (auto activatedRes = implementation->activate(); !activatedRes)
      {
        retire();
        return activatedRes;
      }

      // The explicit successor root becomes durable only after both the native
      // window and its process-wide adapters are active. A failed save leaves
      // the usable process live and later settings saves retry the in-memory root.
      if (auto committedRes = _sessionPtr->commitSelectedRoot(); !committedRes)
      {
        APP_LOG_WARN("LibraryWindowSession: failed to persist the selected library: {}", committedRes.error().message);
      }

      if (_sessionPtr->scanAfterOpen())
      {
        _sessionPtr->rescan();
      }

      return {};
    }
    catch (std::bad_alloc const&)
    {
      retire();
      throw;
    }
    catch (winrt::hresult_error const& error)
    {
      retire();
      return std::unexpected{hresultError("Failed to start the WinUI library window session", error)};
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

    auto* const implementation = winrt::get_self<MainWindow>(window.as<winrt::Aobus::MainWindow>());
    implementation->retire();
    window.Close();

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
    try
    {
      if (!_window)
      {
        return;
      }

      _windowClosedRevoker.revoke();
      auto closedWindow = std::move(_window);

      auto* const implementation = winrt::get_self<MainWindow>(closedWindow.as<winrt::Aobus::MainWindow>());
      implementation->retire();

      closedWindow = nullptr;
      releaseSession();

      if (auto onClosed = std::move(_onClosed); onClosed)
      {
        onClosed();
      }
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "WinUI library window close callback");
    }
  }
} // namespace ao::winui
