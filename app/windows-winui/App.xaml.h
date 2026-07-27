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
    ~App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

  private:
    Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
    std::unique_ptr<ao::winui::LibrarySession> _sessionPtr;
    Microsoft::UI::Xaml::Window _window{nullptr};
    event_token _windowClosedToken{};
    bool _hasWindowClosedToken = false;
  };
}
