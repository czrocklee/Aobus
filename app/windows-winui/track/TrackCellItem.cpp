// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCellItem.h"

#if __has_include("TrackCellItem.g.cpp")
#include "TrackCellItem.g.cpp"
#endif

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnDefaults.h>

#include <winrt/Microsoft.UI.Xaml.Data.h>

#include <utility>

namespace winrt::Aobus::implementation
{
  TrackCellItem::TrackCellItem(hstring text,
                               hstring fieldId,
                               double const width,
                               bool const sortable,
                               hstring resizeName)
    : _text{std::move(text)}
    , _fieldId{std::move(fieldId)}
    , _width{width}
    , _sortable{sortable}
    , _resizeName{std::move(resizeName)}
  {
    if (auto const optField = ao::rt::trackFieldFromId(to_string(_fieldId));
        optField && ao::uimodel::trackColumnDefaults(*optField).alignment == ao::uimodel::TrackColumnAlignment::End)
    {
      _alignment = Microsoft::UI::Xaml::TextAlignment::Right;
    }
  }

  event_token TrackCellItem::PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
  {
    return _propertyChanged.add(handler);
  }

  void TrackCellItem::PropertyChanged(event_token const& token) noexcept
  {
    _propertyChanged.remove(token);
  }

  void TrackCellItem::setText(hstring text)
  {
    if (_text != text)
    {
      _text = std::move(text);
      _propertyChanged(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{L"Text"});
    }
  }
} // namespace winrt::Aobus::implementation
