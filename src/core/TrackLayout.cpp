// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackView.h>

#include <algorithm>
#include <cassert>
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

  std::uint32_t TrackView::hotTagId(std::uint8_t index) const
  {
    auto const& hdr = hotHeader();
    assert(index < hdr.tagLen / sizeof(DictionaryId));
    auto const* tagIds = reinterpret_cast<DictionaryId const*>(_hotData.data() + sizeof(TrackHotHeader));
    return tagIds[index].value();
  }

  std::string_view TrackView::hotGetString(std::uint16_t offset, std::uint16_t len) const
  {
    auto const start = sizeof(TrackHotHeader) + offset;
    assert(start + len <= _hotData.size());
    return utility::asString(_hotData.data(), start, len);
  }

  std::string_view TrackView::coldUri() const
  {
    auto const& hdr = coldHeader();
    auto uriOffset = static_cast<std::uint16_t>(sizeof(TrackColdHeader) + hdr.customLen);
    return coldGetString(uriOffset, hdr.uriLen);
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
    assert(offset + len <= _coldData.size());
    return utility::asString(_coldData.data(), offset, len);
  }

  // TrackView proxy implementations
  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const noexcept
  {
    return std::ranges::find(begin(), end(), tagIdToCheck) != end();
  }

  std::optional<std::string_view> TrackView::CustomProxy::get(std::string_view key) const
  {
    for (auto const& [k, v] : *this)
    {
      if (k == key) { return v; }
    }

    return std::nullopt;
  }

  TrackView::CustomProxy::Iterator TrackView::CustomProxy::begin() const
  {
    auto const& hdr = _track.coldHeader();
    constexpr std::size_t kHeaderSize = sizeof(TrackColdHeader);
    auto const* customStart = _track._coldData.data() + kHeaderSize;
    auto const* customEnd = customStart + hdr.customLen;
    return CustomProxy::Iterator{customStart, customEnd};
  }

  TrackView::CustomProxy::Iterator TrackView::CustomProxy::end() const
  {
    auto const& hdr = _track.coldHeader();
    constexpr std::size_t kHeaderSize = sizeof(TrackColdHeader);
    auto const* customEnd = _track._coldData.data() + kHeaderSize + hdr.customLen;
    return CustomProxy::Iterator{customEnd, customEnd};
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

    if (static_cast<std::size_t>(end - ptr) < kLengthFieldsSize) { return false; }

    std::uint16_t keyLen = 0;
    std::uint16_t valueLen = 0;
    std::memcpy(&keyLen, ptr, sizeof(keyLen));
    ptr += sizeof(keyLen);
    std::memcpy(&valueLen, ptr, sizeof(valueLen));
    ptr += sizeof(valueLen);

    auto const payloadLen = static_cast<std::size_t>(keyLen) + static_cast<std::size_t>(valueLen);
    auto const payloadAvailable = static_cast<std::size_t>(end - ptr);

    if (payloadLen > payloadAvailable) { return false; }

    std::string_view key = utility::asString(std::span{ptr, static_cast<std::size_t>(keyLen)});
    ptr += keyLen;
    std::string_view value = utility::asString(std::span{ptr, static_cast<std::size_t>(valueLen)});
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
