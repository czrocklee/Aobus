// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/WinUiDependencies.h"
#include <ao/Error.h>

#include <functional>
#include <memory>
#include <string>

namespace ao::winui
{
  class LibrarySession;

  struct WindowsUiCoordinatorCallbacks final
  {
    std::function<void()> onTrackListChanged;
    std::move_only_function<void() noexcept> onRuntimeChanging;
    std::move_only_function<void() noexcept> onRuntimeChanged;
    std::function<void(std::string)> onStatus;
    std::function<void(Error const&)> onFailure;
  };

  class WindowsUiCoordinator final
  {
  public:
    WindowsUiCoordinator(LibrarySession& session,
                         WindowsUiViewDependencies views,
                         WindowsUiCoordinatorCallbacks callbacks);
    ~WindowsUiCoordinator();

    WindowsUiCoordinator(WindowsUiCoordinator const&) = delete;
    WindowsUiCoordinator& operator=(WindowsUiCoordinator const&) = delete;
    WindowsUiCoordinator(WindowsUiCoordinator&&) = delete;
    WindowsUiCoordinator& operator=(WindowsUiCoordinator&&) = delete;

    WinUiDependencies uiDependencies() const;
    void retire();

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::winui
