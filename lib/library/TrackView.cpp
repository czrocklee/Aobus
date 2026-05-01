// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/library/TrackView.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstring>

namespace rs::library
{
  // TrackView member implementations
  std::string_view TrackView::hotTitle() const
  {
    auto const& hdr = hotHeader();
    auto titleOffset = hdr.tagLen; // title comes after tags
    return hotGetString(titleOffset, hdr.titleLen);
  }

  DictionaryId TrackView::hotTagId(std::uint8_t index) const
  {
    auto const& hdr = hotHeader();
    gsl_Expects(index < hdr.tagLen / sizeof(DictionaryId));
    auto tagIds = utility::layout::viewArray<DictionaryId>(_hotData.subspan(sizeof(TrackHotHeader), hdr.tagLen));
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
    return coldGetString(hdr.uriOffset, hdr.uriLen);
  }

  std::uint64_t TrackView::coldFileSize() const noexcept
  {
    auto const& hdr = coldHeader();
    return utility::uint64Parts::combine(hdr.fileSizeLo, hdr.fileSizeHi);
  }

  std::uint64_t TrackView::coldMtime() const noexcept
  {
    auto const& hdr = coldHeader();
    return utility::uint64Parts::combine(hdr.mtimeLo, hdr.mtimeHi);
  }

  std::string_view TrackView::coldGetString(std::uint16_t offset, std::uint16_t len) const
  {
    gsl_Expects(offset + len <= _coldData.size());
    return utility::bytes::stringView(_coldData.subspan(offset, len));
  }

  // TrackView proxy implementations
  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const noexcept
  {
    return std::ranges::contains(begin(), end(), tagIdToCheck);
  }

  std::optional<std::string_view> TrackView::CustomProxy::get(DictionaryId dictId) const
  {
    // Threshold for switching between linear and binary search
    constexpr std::size_t kSearchThreshold = 64;

    auto customEntries = entries();

    // Small N: linear search via ranges::find_if (cache-friendly, no divisions)

    if (customEntries.size() < kSearchThreshold)
    {
      if (auto it = std::ranges::find(customEntries, dictId, &Entry::dictId); it != customEntries.end())
      {
        gsl_Expects(it->offset + it->len <= _coldData.size());
        return utility::bytes::stringView(_coldData.subspan(it->offset, it->len));
      }

      return std::nullopt;
    }

    // Large N: binary search via ranges::lower_bound

    if (auto it = std::ranges::lower_bound(customEntries, dictId, {}, &Entry::dictId);
        it != customEntries.end() && it->dictId == dictId)
    {
      gsl_Expects(it->offset + it->len <= _coldData.size());
      return utility::bytes::stringView(_coldData.subspan(it->offset, it->len));
    }

    return std::nullopt;
  }

  TrackView::CustomProxy::Iterator TrackView::CustomProxy::begin() const
  {
    auto customEntries = entries();
    return {customEntries.data(), _coldData.data()};
  }

  TrackView::CustomProxy::Iterator TrackView::CustomProxy::end() const
  {
    auto customEntries = entries();
    return {customEntries.data() + customEntries.size(), _coldData.data()};
  }

  TrackView::CustomProxy::Iterator::Iterator(Entry const* pos, std::byte const* coldDataBase)
    : _pos{pos}, _coldDataBase{coldDataBase}
  {
  }

  TrackView::CustomProxy::Iterator::value_type TrackView::CustomProxy::Iterator::operator*() const
  {
    auto const& entry = *_pos;
    auto value = std::string_view{};

    if (entry.len > 0)
    {
      value = utility::bytes::stringView(utility::bytes::view(_coldDataBase + entry.offset, entry.len));
    }

    return {entry.dictId, value};
  }

  TrackView::CustomProxy::Iterator& TrackView::CustomProxy::Iterator::operator++()
  {
    ++_pos;
    return *this;
  }

  TrackView::CustomProxy::Iterator TrackView::CustomProxy::Iterator::operator++(int)
  {
    auto tmp = *this;
    ++_pos;
    return tmp;
  }
} // namespace rs::library
