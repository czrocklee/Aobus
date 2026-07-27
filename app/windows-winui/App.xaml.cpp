// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "App.xaml.h"

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "pch.h"
#include "platform/WindowsStringResources.h"
#include <ao/rt/Log.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace winrt::Aobus::implementation
{
  namespace
  {
    std::filesystem::path stateRoot()
    {
      auto buffer = std::array<wchar_t, 32'768>{};
      auto const length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));

      if (length == 0 || length >= buffer.size())
      {
        throw std::runtime_error{"LOCALAPPDATA is unavailable"};
      }

      return std::filesystem::path{std::wstring_view{buffer.data(), length}} / "Aobus";
    }

    void showStartupFailure(std::string_view const detail) noexcept
    {
      try
      {
        auto const message = ao::winui::formatResource("StartupFailureFormat", detail);
        auto const messageText = winrt::to_hstring(message);
        auto const title = ao::winui::resourceHstring(L"AppTitleValue");
        MessageBoxW(nullptr, messageText.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
      }
      catch (...)
      {
        MessageBoxW(nullptr, L"Aobus could not start.", L"Aobus", MB_OK | MB_ICONERROR);
      }
    }
  } // namespace

  App::App()
  {
    InitializeComponent();
  }

  App::~App() = default;

  void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
  {
    auto const failLaunch = [this](std::string detail) noexcept
    {
      try
      {
        APP_LOG_CRITICAL("WinUI startup failed: {}", detail);
      }
      catch (...)
      {
      }
      try
      {
        if (_window && _hasWindowClosedToken)
        {
          _window.Closed(_windowClosedToken);
        }
        _hasWindowClosedToken = false;
      }
      catch (...)
      {
        _hasWindowClosedToken = false;
      }

      try
      {
        if (_window)
        {
          winrt::get_self<MainWindow>(_window.as<winrt::Aobus::MainWindow>())->retire();
        }
      }
      catch (...)
      {
      }

      _window = nullptr;
      _sessionPtr.reset();
      _dispatcher = nullptr;
      showStartupFailure(detail);
      try
      {
        Microsoft::UI::Xaml::Application::Current().Exit();
      }
      catch (...)
      {
        PostQuitMessage(1);
      }
    };

    try
    {
      _dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
      _sessionPtr = std::make_unique<ao::winui::LibrarySession>(stateRoot(), _dispatcher);
      auto mainWindow = winrt::make<MainWindow>();
      auto* const mainWindowPtr = winrt::get_self<MainWindow>(mainWindow);
      mainWindowPtr->initialize(*_sessionPtr);
      _window = mainWindow;
      _windowClosedToken = _window.Closed(
        [this, mainWindowPtr](
          Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::WindowEventArgs const&)
        {
          if (_hasWindowClosedToken)
          {
            sender.as<Microsoft::UI::Xaml::Window>().Closed(_windowClosedToken);
            _hasWindowClosedToken = false;
          }

          mainWindowPtr->retire();
          _sessionPtr.reset();
          _window = nullptr;
          _dispatcher = nullptr;
        });
      _hasWindowClosedToken = true;
      _window.Activate();
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
