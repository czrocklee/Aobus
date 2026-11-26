// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <cstddef>
#include <functional>

namespace ao::winui
{
  using TrackItemProvider = std::function<winrt::Windows::Foundation::IInspectable(std::size_t)>;

  // The returned view also exposes IVector for indexed ListView access, but
  // rejects membership and order changes. Its ISelectionInfo is mutable and
  // tracks selection by index without materializing rows through the provider.
  winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable>
  makeTrackItemView(std::size_t size, TrackItemProvider provider, std::size_t maximumEntries);
} // namespace ao::winui
