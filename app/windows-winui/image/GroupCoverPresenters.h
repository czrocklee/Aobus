// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <winrt/Windows.Foundation.h>

#include <memory>

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class ResourceByteMemoryCache;
}

namespace ao::winui
{
  class ThemeCoordinator;

  /// Owns cover presenters for one window's group-heading tiles on the XAML thread.
  /// The runtime, resource cache, and theme must outlive this owner.
  class GroupCoverPresenters final
  {
  public:
    GroupCoverPresenters(async::Runtime& runtime, rt::ResourceByteMemoryCache& resources, ThemeCoordinator& theme);
    ~GroupCoverPresenters();

    GroupCoverPresenters(GroupCoverPresenters const&) = delete;
    GroupCoverPresenters& operator=(GroupCoverPresenters const&) = delete;
    GroupCoverPresenters(GroupCoverPresenters&&) = delete;
    GroupCoverPresenters& operator=(GroupCoverPresenters&&) = delete;

    void refresh(winrt::Windows::Foundation::IInspectable const& sender);
    void unload(winrt::Windows::Foundation::IInspectable const& sender);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::winui
