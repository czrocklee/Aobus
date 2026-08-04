// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <functional>
#include <memory>
#include <string>

namespace ao::rt
{
  class ResourceByteLoader;
}

namespace ao::winui
{
  class LibrarySession;
  class ThemeCoordinator;
  class TrackListController;

  struct UiCoordinatorCallbacks final
  {
    std::function<void(std::string)> onStatus;
    std::function<void(Error const&)> onFailure;
  };

  class UiCoordinator final
  {
  public:
    UiCoordinator(LibrarySession& session, UiCoordinatorCallbacks callbacks);
    ~UiCoordinator();

    UiCoordinator(UiCoordinator const&) = delete;
    UiCoordinator& operator=(UiCoordinator const&) = delete;
    UiCoordinator(UiCoordinator&&) = delete;
    UiCoordinator& operator=(UiCoordinator&&) = delete;

    TrackListController& trackList() const noexcept;
    ThemeCoordinator& theme() const noexcept;
    rt::ResourceByteLoader& resourceBytes() const noexcept;

  private:
    /// Disable callback publication and release borrowed models. Destructor-only.
    void retire() noexcept;

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::winui
