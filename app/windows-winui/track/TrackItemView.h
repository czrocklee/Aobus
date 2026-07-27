// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <cstddef>
#include <functional>

namespace ao::winui
{
  using TrackItemProvider = std::function<winrt::Windows::Foundation::IInspectable(std::size_t)>;

  winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable>
  makeTrackItemView(std::size_t size, TrackItemProvider provider, std::size_t maximumEntries);
} // namespace ao::winui
