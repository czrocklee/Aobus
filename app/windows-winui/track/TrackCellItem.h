// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackCellItem.g.h"

#include <winrt/Microsoft.UI.Xaml.h>

namespace winrt::Aobus::implementation
{
  struct TrackCellItem : TrackCellItemT<TrackCellItem>
  {
    TrackCellItem(hstring text, hstring fieldId, double width, bool sortable);

    hstring Text() const noexcept { return _text; }
    hstring FieldId() const noexcept { return _fieldId; }
    double Width() const noexcept { return _width; }
    bool Sortable() const noexcept { return _sortable; }
    Microsoft::UI::Xaml::TextAlignment Alignment() const noexcept { return _alignment; }

  private:
    hstring _text;
    hstring _fieldId;
    double _width = 0.0;
    bool _sortable = false;
    Microsoft::UI::Xaml::TextAlignment _alignment = Microsoft::UI::Xaml::TextAlignment::Left;
  };
} // namespace winrt::Aobus::implementation

namespace winrt::Aobus::factory_implementation
{
  struct TrackCellItem : TrackCellItemT<TrackCellItem, implementation::TrackCellItem>
  {};
} // namespace winrt::Aobus::factory_implementation
