// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackView.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstring>

namespace rs::core
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
    auto tagIds = utility::asArray<DictionaryId>(_hotData.subspan(sizeof(TrackHotHeader)));
    return tagIds[index];
  }

  std::string_view TrackView::hotGetString(std::uint16_t offset, std::uint16_t length) const
  {
    auto const start = sizeof(TrackHotHeader) + offset;
    gsl_Expects(start + length <= _hotData.size());
    return utility::asString(_hotData.data(), start, length);
  }

  std::string_view TrackView::coldUri() const
  {
    auto const& hdr = coldHeader();
    return coldGetString(hdr.uriOffset, hdr.uriLen);
  }

  std::uint64_t TrackView::coldFileSize() const noexcept
  {
    auto const& hdr = coldHeader();
    return utility::combineInt64(hdr.fileSizeLo, hdr.fileSizeHi);
  }

  std::uint64_t TrackView::coldMtime() const noexcept
  {
    auto const& hdr = coldHeader();
    return utility::combineInt64(hdr.mtimeLo, hdr.mtimeHi);
  }

  std::string_view TrackView::coldGetString(std::uint16_t offset, std::uint16_t len) const
  {
    gsl_Expects(offset + len <= _coldData.size());
    return utility::asString(_coldData.data(), offset, len);
  }

  // TrackView proxy implementations
  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const noexcept
  {
    return std::ranges::find(begin(), end(), tagIdToCheck) != end();
  }

  std::optional<std::string_view> TrackView::CustomProxy::get([[maybe_unused]] std::string_view key) const
  {
    // With indexed format, keys are stored as DictionaryIds, not strings.
    // Callers should use get(DictionaryId) after resolving the string to a dictId.
    // This method is kept for API compatibility but requires dictionary resolution by caller.
    return std::nullopt;
  }

  std::optional<std::string_view> TrackView::CustomProxy::get(DictionaryId dictId) const
  {
    auto const& hdr = _track.coldHeader();
    constexpr std::size_t kHeaderSize = sizeof(TrackColdHeader);

    // Threshold for switching between linear and binary search
    constexpr std::size_t kSearchThreshold = 64;

    auto entries = utility::asArray<Entry>(_track._coldData.subspan(kHeaderSize)).first(hdr.customCount);

    // Small N: linear search via ranges::find_if (cache-friendly, no divisions)
    if (hdr.customCount < kSearchThreshold)
    {
      if (auto it = std::ranges::find(entries, dictId, &Entry::dictId); it != entries.end())
      {
        gsl_Expects(it->offset + it->len <= _track._coldData.size());
        return utility::asString(_track._coldData.data(), it->offset, it->len);
      }

      return std::nullopt;
    }

    // Large N: binary search via ranges::lower_bound
    if (auto it = std::ranges::lower_bound(entries, dictId, {}, &Entry::dictId);
        it != entries.end() && it->dictId == dictId)
    {
      gsl_Expects(it->offset + it->len <= _track._coldData.size());
      return utility::asString(_track._coldData.data(), it->offset, it->len);
    }

    return std::nullopt;
  }

  TrackView::CustomProxy::Iterator TrackView::CustomProxy::begin() const
  {
    constexpr std::size_t kHeaderSize = sizeof(TrackColdHeader);
    auto entries = utility::asArray<Entry>(_track._coldData.subspan(kHeaderSize));
    return {entries.data(), _track._coldData.data()};
  }

  TrackView::CustomProxy::Iterator TrackView::CustomProxy::end() const
  {
    auto const& hdr = _track.coldHeader();
    constexpr std::size_t kHeaderSize = sizeof(TrackColdHeader);
    auto entries = utility::asArray<Entry>(_track._coldData.subspan(kHeaderSize));
    return {entries.subspan(hdr.customCount).data(), _track._coldData.data()};
  }

  TrackView::CustomProxy::Iterator::Iterator(Entry const* pos, std::byte const* coldDataBase)
    : _pos(pos)
    , _coldDataBase(coldDataBase)
  {
  }

  std::pair<DictionaryId, std::string_view> const& TrackView::CustomProxy::Iterator::dereference() const
  {
    auto const& entry = *_pos;
    std::string_view value;
    if (entry.len > 0) { value = utility::asString(_coldDataBase, entry.offset, entry.len); }
    _currentValue = {entry.dictId, value};
    return _currentValue;
  }

  bool TrackView::CustomProxy::Iterator::equal(Iterator const& other) const
  {
    return _pos == other._pos;
  }

  void TrackView::CustomProxy::Iterator::increment()
  {
    ++_pos;
  }

} // namespace rs::core
