// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackRowItem.h"

#include "platform/StringResourceFormat.h"
#include "track/TrackCellItem.h"
#include <ao/i18n/MessageCatalog.h>

#if __has_include("TrackRowItem.g.cpp")
#include "TrackRowItem.g.cpp"
#endif

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackRowField.h>

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace winrt::Aobus::implementation
{
  namespace
  {
    constexpr double kDefaultTitleWidth = 320.0;
    constexpr double kDefaultMetadataWidth = 220.0;
    constexpr double kDefaultDurationWidth = 80.0;

    using TrackCellsProjection = decltype(std::declval<TrackRowItem const&>().Cells());

    static_assert(std::same_as<TrackCellsProjection,
                               Windows::Foundation::Collections::IVectorView<Windows::Foundation::IInspectable>>,
                  "TrackRowItem.Cells must expose inspectable elements for ItemsControl.ItemsSource");

    std::string trackTitle(ao::rt::TrackRow const& row)
    {
      if (auto optTitle = ao::uimodel::trackTitleOrFilename(row); optTitle)
      {
        return std::move(*optTitle);
      }

      return ao::winui::formatResource("track_fallback", row.id.raw());
    }

    void appendCell(Windows::Foundation::Collections::IVector<Windows::Foundation::IInspectable> const& cells,
                    hstring text,
                    std::string_view const fieldId,
                    double const width,
                    bool const sortable)
    {
      cells.Append(make<TrackCellItem>(std::move(text), to_hstring(std::string{fieldId}), width, sortable, hstring{}));
    }
  } // namespace

  TrackRowItem::TrackRowItem(std::uint32_t const index,
                             std::uint32_t const trackId,
                             std::uint32_t const coverArtId,
                             hstring title,
                             hstring artist,
                             hstring album,
                             hstring duration)
    : _displayIndex{index}
    , _index{index}
    , _trackId{trackId}
    , _coverArtId{coverArtId}
    , _title{std::move(title)}
    , _artist{std::move(artist)}
    , _album{std::move(album)}
  {
    appendCell(_cells, _title, "title", kDefaultTitleWidth, true);
    appendCell(_cells, _artist, "artist", kDefaultMetadataWidth, true);
    appendCell(_cells, _album, "album", kDefaultMetadataWidth, true);
    appendCell(_cells, std::move(duration), "duration", kDefaultDurationWidth, true);
  }

  TrackRowItem::TrackRowItem(std::uint32_t const displayIndex,
                             std::uint32_t const sourceIndex,
                             ao::rt::TrackRow const& row,
                             ao::i18n::MessageCatalog const& textCatalog,
                             std::span<ao::winui::TrackColumnCellSpec const> const columns)
    : _displayIndex{displayIndex}
    , _index{sourceIndex}
    , _trackId{row.id.raw()}
    , _coverArtId{row.coverArtId.raw()}
    , _title{winrt::to_hstring(trackTitle(row))}
    , _artist{winrt::to_hstring(row.artist)}
    , _album{winrt::to_hstring(row.album)}
  {
    for (auto const& column : columns)
    {
      auto const* definition = ao::rt::trackFieldDefinition(column.field);
      auto text = column.field == ao::rt::TrackField::Title
                    ? _title
                    : to_hstring(ao::uimodel::formatTrackRowField(
                        textCatalog, row, column.field, ao::uimodel::TrackRowPathFormat::Generic));
      appendCell(_cells,
                 std::move(text),
                 ao::rt::trackFieldId(column.field),
                 column.width,
                 definition != nullptr && definition->optSortField);
    }
  }

  TrackRowItem::TrackRowItem(std::uint32_t const displayIndex,
                             std::uint32_t const sourceIndex,
                             std::uint32_t const coverArtId,
                             std::string groupCountText,
                             std::string primary,
                             std::string secondary,
                             std::string tertiary,
                             std::string coverArtMonogram)
    : _displayIndex{displayIndex}
    , _index{sourceIndex}
    , _coverArtId{coverArtId}
    , _isGroupHeader{true}
    , _title{winrt::to_hstring(primary)}
    , _artist{winrt::to_hstring(secondary)}
    , _album{winrt::to_hstring(tertiary)}
    , _groupCountText{winrt::to_hstring(groupCountText)}
    , _coverArtMonogram{winrt::to_hstring(coverArtMonogram)}
  {
  }
} // namespace winrt::Aobus::implementation
