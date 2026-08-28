// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCellItem.h"

#include "pch.h"

#if __has_include("TrackCellItem.g.cpp")
#include "TrackCellItem.g.cpp"
#endif

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnDefaults.h>

#include <utility>

namespace winrt::Aobus::implementation
{
  TrackCellItem::TrackCellItem(hstring text, hstring fieldId, double const width, bool const sortable)
    : _text{std::move(text)}, _fieldId{std::move(fieldId)}, _width{width}, _sortable{sortable}
  {
    if (auto const optField = ao::rt::trackFieldFromId(to_string(_fieldId));
        optField && ao::uimodel::trackColumnDefaults(*optField).alignment == ao::uimodel::TrackColumnAlignment::End)
    {
      _alignment = Microsoft::UI::Xaml::TextAlignment::Right;
    }
  }
} // namespace winrt::Aobus::implementation
