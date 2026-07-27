// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackRowItem.g.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>

#include <winrt/Windows.Foundation.Collections.h>

#include <cstdint>
#include <span>
#include <string>

namespace ao::winui
{
  struct TrackColumnCellSpec final
  {
    rt::TrackField field = rt::TrackField::Title;
    double width = 0.0;
  };
} // namespace ao::winui

namespace winrt::Aobus::implementation
{
  struct TrackRowItem : TrackRowItemT<TrackRowItem>
  {
    TrackRowItem(std::uint32_t index,
                 std::uint32_t trackId,
                 std::uint32_t coverArtId,
                 hstring title,
                 hstring artist,
                 hstring album,
                 hstring duration);
    TrackRowItem(std::uint32_t displayIndex,
                 std::uint32_t sourceIndex,
                 ao::rt::TrackRow const& row,
                 std::span<ao::winui::TrackColumnCellSpec const> columns);
    TrackRowItem(std::uint32_t displayIndex,
                 std::uint32_t sourceIndex,
                 std::uint32_t coverArtId,
                 std::uint32_t groupCount,
                 std::string primary,
                 std::string secondary,
                 std::string tertiary,
                 std::string coverArtMonogram);

    std::uint32_t DisplayIndex() const noexcept { return _displayIndex; }
    std::uint32_t Index() const noexcept { return _index; }
    std::uint32_t TrackId() const noexcept { return _trackId; }
    std::uint32_t CoverArtId() const noexcept { return _coverArtId; }
    bool IsGroupHeader() const noexcept { return _isGroupHeader; }
    double RowHeight() const noexcept { return _isGroupHeader ? 0.0 : 32.0; }
    double GroupHeight() const noexcept { return _isGroupHeader ? 72.0 : 0.0; }
    std::uint32_t GroupCount() const noexcept { return _groupCount; }
    hstring Title() const noexcept { return _title; }
    hstring Artist() const noexcept { return _artist; }
    hstring Album() const noexcept { return _album; }
    hstring CoverArtMonogram() const noexcept { return _coverArtMonogram; }
    Windows::Foundation::Collections::IVectorView<Windows::Foundation::IInspectable> Cells() const
    {
      return _cells.GetView();
    }

  private:
    std::uint32_t _displayIndex = 0;
    std::uint32_t _index = 0;
    std::uint32_t _trackId = 0;
    std::uint32_t _coverArtId = 0;
    std::uint32_t _groupCount = 0;
    bool _isGroupHeader = false;
    hstring _title;
    hstring _artist;
    hstring _album;
    hstring _coverArtMonogram;
    Windows::Foundation::Collections::IVector<Windows::Foundation::IInspectable> _cells{
      single_threaded_vector<Windows::Foundation::IInspectable>()};
  };
} // namespace winrt::Aobus::implementation

namespace winrt::Aobus::factory_implementation
{
  struct TrackRowItem : TrackRowItemT<TrackRowItem, implementation::TrackRowItem>
  {};
} // namespace winrt::Aobus::factory_implementation
