// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackCellItem.g.h"

#include <winrt/Microsoft.UI.Xaml.h>

namespace winrt::Aobus::implementation
{
  // Generated C++/WinRT virtuals implement the platform ABI and are validated by the native MSVC build.
  // NOLINTNEXTLINE(portability-template-virtual-member-function)
  struct TrackCellItem : TrackCellItemT<TrackCellItem>
  {
    TrackCellItem(hstring text, hstring fieldId, double width, bool sortable, hstring resizeName);

    // Project.idl fixes these property names for the generated TrackCellItem projection.
    // NOLINTNEXTLINE(readability-identifier-naming)
    hstring Text() const noexcept { return _text; }
    // NOLINTNEXTLINE(readability-identifier-naming)
    hstring FieldId() const noexcept { return _fieldId; }
    // NOLINTNEXTLINE(readability-identifier-naming)
    double Width() const noexcept { return _width; }
    // NOLINTNEXTLINE(aobus-readability-bool-function-naming,readability-identifier-naming)
    bool Sortable() const noexcept { return _sortable; }
    /// The accessible name of a header cell's resize gripper; body cells have none.
    // NOLINTNEXTLINE(readability-identifier-naming)
    hstring ResizeName() const noexcept { return _resizeName; }
    // NOLINTNEXTLINE(readability-identifier-naming)
    Microsoft::UI::Xaml::TextAlignment Alignment() const noexcept { return _alignment; }

  private:
    hstring _text;
    hstring _fieldId;
    double _width = 0.0;
    bool _sortable = false;
    hstring _resizeName;
    Microsoft::UI::Xaml::TextAlignment _alignment = Microsoft::UI::Xaml::TextAlignment::Left;
  };
} // namespace winrt::Aobus::implementation

namespace winrt::Aobus::factory_implementation
{
  // Generated C++/WinRT factory virtuals implement the ABI and are validated by the native MSVC build.
  // NOLINTNEXTLINE(portability-template-virtual-member-function)
  struct TrackCellItem : TrackCellItemT<TrackCellItem, implementation::TrackCellItem>
  {};
} // namespace winrt::Aobus::factory_implementation
