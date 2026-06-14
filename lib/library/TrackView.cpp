// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/utility/ByteView.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::library
{
  // TrackView member implementations
  TrackHotHeader const& TrackView::hotHeader() const
  {
    gsl_Expects(isHotValid());

    auto const* header = utility::layout::view<TrackHotHeader>(_hotData);
    gsl_Assert(header != nullptr);

    if (header == nullptr)
    {
      std::unreachable();
    }

    return *header;
  }

  TrackColdHeader const& TrackView::coldHeader() const
  {
    gsl_Expects(isColdValid());

    auto const* header = utility::layout::view<TrackColdHeader>(_coldData);
    gsl_Assert(header != nullptr);

    if (header == nullptr)
    {
      std::unreachable();
    }

    return *header;
  }

  std::string_view TrackView::hotTitle() const
  {
    auto const& hdr = hotHeader();
    auto titleOffset = hdr.tagLength; // title comes after tags
    return hotGetString(titleOffset, hdr.titleLength);
  }

  DictionaryId TrackView::hotTagId(std::uint8_t index) const
  {
    auto const& hdr = hotHeader();
    gsl_Expects(index < hdr.tagLength / sizeof(DictionaryId));
    auto tagIds = utility::layout::viewArray<DictionaryId>(_hotData.subspan(sizeof(TrackHotHeader), hdr.tagLength));
    return tagIds[index];
  }

  std::string_view TrackView::hotGetString(std::uint16_t offset, std::uint16_t length) const
  {
    auto const start = sizeof(TrackHotHeader) + offset;
    gsl_Expects(start + length <= _hotData.size());
    return utility::bytes::stringView(_hotData.subspan(start, length));
  }

  std::string_view TrackView::coldUri() const
  {
    auto const& hdr = coldHeader();
    return coldGetString(hdr.uriOffset, hdr.uriLength);
  }

  std::string_view TrackView::coldGetString(std::uint16_t offset, std::uint16_t len) const
  {
    gsl_Expects(offset + len <= _coldData.size());
    return utility::bytes::stringView(_coldData.subspan(offset, len));
  }

  // TrackView proxy implementations
  TrackHotHeader const& TrackView::TagProxy::hotHeader() const
  {
    gsl_Expects(_hotData.size() >= sizeof(TrackHotHeader));

    auto const* header = utility::layout::view<TrackHotHeader>(_hotData);
    gsl_Assert(header != nullptr);

    if (header == nullptr)
    {
      std::unreachable();
    }

    return *header;
  }

  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const noexcept
  {
    return std::ranges::contains(begin(), end(), tagIdToCheck);
  }

  std::optional<CoverArt> TrackView::CoverArtProxy::primary() const noexcept
  {
    auto const coverCount = count();

    if (coverCount == 0)
    {
      return std::nullopt;
    }

    auto const* entriesData = entries().data();

    for (std::uint16_t i = 0; i < coverCount; ++i)
    {
      if (static_cast<PictureType>(entriesData[i].type) == PictureType::FrontCover)
      {
        return at(i);
      }
    }

    return at(0);
  }

  TrackView::CoverArtProxy::Iterator TrackView::CoverArtProxy::begin() const
  {
    return Iterator{entries().data()};
  }

  TrackView::CoverArtProxy::Iterator TrackView::CoverArtProxy::end() const
  {
    auto const coverEntries = entries();
    return Iterator{coverEntries.data() + coverEntries.size()};
  }

  std::optional<std::string_view> TrackView::CustomMetadataProxy::get(DictionaryId dictId) const
  {
    // Threshold for switching between linear and binary search
    constexpr std::size_t kSearchThreshold = 64;

    auto customEntries = entries();

    // Small N: linear search via ranges::find_if (cache-friendly, no divisions)
    if (customEntries.size() < kSearchThreshold)
    {
      if (auto it = std::ranges::find(customEntries, dictId, &Entry::keyId); it != customEntries.end())
      {
        gsl_Expects(it->valueOffset + it->valueLength <= _coldData.size());
        return utility::bytes::stringView(_coldData.subspan(it->valueOffset, it->valueLength));
      }

      return std::nullopt;
    }

    // Large N: binary search via ranges::lower_bound
    if (auto it = std::ranges::lower_bound(customEntries, dictId, {}, &Entry::keyId);
        it != customEntries.end() && it->keyId == dictId)
    {
      gsl_Expects(it->valueOffset + it->valueLength <= _coldData.size());
      return utility::bytes::stringView(_coldData.subspan(it->valueOffset, it->valueLength));
    }

    return std::nullopt;
  }

  TrackView::CustomMetadataProxy::Iterator TrackView::CustomMetadataProxy::begin() const
  {
    auto customEntries = entries();
    return {customEntries.data(), _coldData.data()};
  }

  TrackView::CustomMetadataProxy::Iterator TrackView::CustomMetadataProxy::end() const
  {
    auto customEntries = entries();
    return {customEntries.data() + customEntries.size(), _coldData.data()};
  }

  TrackView::CustomMetadataProxy::Iterator::Iterator(Entry const* pos, std::byte const* coldDataBase)
    : _pos{pos}, _coldDataBase{coldDataBase}
  {
  }

  TrackView::CustomMetadataProxy::Iterator::value_type TrackView::CustomMetadataProxy::Iterator::operator*() const
  {
    auto const& entry = *_pos;
    auto value = std::string_view{};

    if (entry.valueLength > 0)
    {
      value = utility::bytes::stringView(utility::bytes::view(_coldDataBase + entry.valueOffset, entry.valueLength));
    }

    return {entry.keyId, value};
  }

  TrackView::CustomMetadataProxy::Iterator& TrackView::CustomMetadataProxy::Iterator::operator++()
  {
    ++_pos;
    return *this;
  }

  TrackView::CustomMetadataProxy::Iterator TrackView::CustomMetadataProxy::Iterator::operator++(std::int32_t)
  {
    auto tmp = Iterator{*this};
    ++_pos;
    return tmp;
  }
} // namespace ao::library
