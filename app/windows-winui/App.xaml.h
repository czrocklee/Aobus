// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "App.xaml.g.h"

#include <memory>

namespace ao::winui
{
  class LibrarySession;
}

namespace winrt::Aobus::implementation
{
  struct App : AppT<App>
  {
    App();
    ~App() override;

    App(App const&) = delete;
    App& operator=(App const&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& /*args*/);

  private:
    Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
    std::unique_ptr<ao::winui::LibrarySession> _sessionPtr;
    Microsoft::UI::Xaml::Window _window{nullptr};
    event_token _windowClosedToken{};
    bool _hasWindowClosedToken = false;
  };
} // namespace winrt::Aobus::implementation
