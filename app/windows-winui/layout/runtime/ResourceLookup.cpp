// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ResourceLookup.h"

#include "pch.h"

#include <string_view>

namespace ao::winui::layout
{
  winrt::Windows::Foundation::IInspectable lookupResource(
    winrt::Microsoft::UI::Xaml::ResourceDictionary const& resources,
    std::string_view const key)
  {
    auto const boxed = winrt::box_value(winrt::to_hstring(key));

    if (resources && resources.HasKey(boxed))
    {
      return resources.Lookup(boxed);
    }

    auto const application = winrt::Microsoft::UI::Xaml::Application::Current();

    if (application && application.Resources() && application.Resources().HasKey(boxed))
    {
      return application.Resources().Lookup(boxed);
    }

    return nullptr;
  }
} // namespace ao::winui::layout
