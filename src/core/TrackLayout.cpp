// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackView.h>

#include <algorithm>
#include <cstring>

namespace rs::core
{

  // TrackView member implementations
  std::string_view TrackView::hotTitle() const
  {
    auto* hdr = hotHeader();
    if (!hdr) { return {}; }
    auto titleOffset = hdr->tagLen; // title comes after tags
    return hotGetString(titleOffset, hdr->titleLen);
  }

  std::uint32_t TrackView::hotTagId(std::uint8_t index) const
  {
    auto* hdr = hotHeader();
    if (!hdr) { return 0; }
    auto tagCount = hdr->tagLen / sizeof(DictionaryId);
    if (index >= tagCount) { return 0; }
    auto offset = sizeof(TrackHotHeader) + index * sizeof(DictionaryId);
    if (_hotData.size() < offset + sizeof(DictionaryId)) { return 0; }
    std::uint32_t result;
    std::memcpy(&result, utility::as<std::uint32_t>(hdr, offset), sizeof(std::uint32_t));
    return result;
  }

  std::string_view TrackView::hotGetString(std::uint16_t offset, std::uint16_t len) const
  {
    if (len == 0) { return {}; }
    std::size_t const start = sizeof(TrackHotHeader) + offset;
    if (start + len > _hotData.size()) { return {}; }
    return {utility::as<char>(_hotData.data(), start), len};
  }

  std::string_view TrackView::coldUri() const
  {
    auto* hdr = coldHeader();
    if (!hdr) { return {}; }
    auto uriOffset = static_cast<std::uint16_t>(sizeof(TrackColdHeader) + hdr->customLen);
    return coldGetString(uriOffset, hdr->uriLen);
  }

  std::uint64_t TrackView::coldFileSize() const noexcept
  {
    auto* hdr = coldHeader();
    if (!hdr) { return 0; }
    return utility::combineInt64(hdr->fileSizeLo, hdr->fileSizeHi);
  }

  std::uint64_t TrackView::coldMtime() const noexcept
  {
    auto* hdr = coldHeader();
    if (!hdr) { return 0; }
    return utility::combineInt64(hdr->mtimeLo, hdr->mtimeHi);
  }

  std::string_view TrackView::coldGetString(std::uint16_t offset, std::uint16_t len) const
  {
    if (!_coldData || len == 0 || offset + len > _coldData->size()) { return {}; }
    auto const* data = _coldData->data();
    return std::string_view{reinterpret_cast<char const*>(data + offset), len};
  }

  // TrackView proxy implementations
  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const noexcept
  {
    auto c = count();
    for (std::uint8_t i = 0; i < c; ++i)
    {
      if (id(i) == tagIdToCheck) { return true; }
    }
    return false;
  }

  std::optional<std::string> TrackView::CustomProxy::get(std::string_view key) const
  {
    for (auto const& [k, v] : *this)
    {
      if (k == key) { return std::string{v}; }
    }
    return std::nullopt;
  }

  std::optional<std::pair<std::byte const*, std::byte const*>> TrackView::CustomProxy::customRange() const
  {
    _track.ensureColdLoaded();
    auto* hdr = _track.coldHeader();
    if (!_track._coldData || !hdr) { return std::nullopt; }

    constexpr std::size_t kHeaderSize = sizeof(TrackColdHeader);
    auto const coldSize = _track._coldData->size();
    if (coldSize < kHeaderSize) { return std::nullopt; }

    auto const customLen = static_cast<std::size_t>(hdr->customLen);
    if (customLen > coldSize - kHeaderSize) { return std::nullopt; }

    auto const* customStart = _track._coldData->data() + kHeaderSize;
    auto const* customEnd = customStart + customLen;
    return std::pair<std::byte const*, std::byte const*>{customStart, customEnd};
  }

  TrackView::CustomProxy::Iterator TrackView::CustomProxy::begin() const
  {
    auto range = customRange();
    if (!range) { return CustomProxy::Iterator{}; }
    return CustomProxy::Iterator{range->first, range->second};
  }

  TrackView::CustomProxy::Iterator TrackView::CustomProxy::end() const
  {
    auto range = customRange();
    if (!range) { return CustomProxy::Iterator{}; }
    return CustomProxy::Iterator{range->second, range->second};
  }

  TrackView::CustomProxy::Iterator::Iterator(std::byte const* data, std::byte const* end)
    : _currentPos(data)
    , _nextPos(data)
    , _end(end)
  {
    if (!_currentPos || !_end || _currentPos >= _end || !decodeEntry(_currentPos, _end, _current, _nextPos))
    {
      _currentPos = _end;
      _nextPos = _end;
      _current = {};
    }
  }

  std::pair<std::string_view, std::string_view> const& TrackView::CustomProxy::Iterator::dereference() const
  {
    return _current;
  }

  bool TrackView::CustomProxy::Iterator::equal(Iterator const& other) const
  {
    return _currentPos == other._currentPos;
  }

  bool TrackView::CustomProxy::Iterator::decodeEntry(std::byte const* ptr,
                                                     std::byte const* end,
                                                     std::pair<std::string_view, std::string_view>& out,
                                                     std::byte const*& next)
  {
    if (!ptr || !end || ptr >= end) { return false; }

    constexpr std::size_t kLengthFieldsSize = sizeof(std::uint16_t) * 2;

    if (auto const available = static_cast<std::size_t>(end - ptr); available < kLengthFieldsSize) { return false; }

    std::uint16_t keyLen = 0;
    std::uint16_t valueLen = 0;
    std::memcpy(&keyLen, ptr, sizeof(keyLen));
    ptr += sizeof(keyLen);
    std::memcpy(&valueLen, ptr, sizeof(valueLen));
    ptr += sizeof(valueLen);

    auto const payloadLen = static_cast<std::size_t>(keyLen) + static_cast<std::size_t>(valueLen);
    auto const payloadAvailable = static_cast<std::size_t>(end - ptr);
    
    if (payloadLen > payloadAvailable) { return false; }

    std::string_view key{reinterpret_cast<char const*>(ptr), static_cast<std::size_t>(keyLen)};
    ptr += keyLen;
    std::string_view value{reinterpret_cast<char const*>(ptr), static_cast<std::size_t>(valueLen)};
    ptr += valueLen;

    out = {key, value};
    next = ptr;
    return true;
  }

  void TrackView::CustomProxy::Iterator::increment()
  {
    if (_currentPos >= _end) { return; }

    _currentPos = _nextPos;

    if (_currentPos >= _end || !decodeEntry(_currentPos, _end, _current, _nextPos))
    {
      _currentPos = _end;
      _nextPos = _end;
      _current = {};
    }
  }

} // namespace rs::core
