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
    return hotGetString(hotHeader()->titleOffset, hotHeader()->titleLen);
  }

  std::uint32_t TrackView::hotTagId(std::uint8_t index) const
  {
    auto* hdr = hotHeader();
    if (!hdr || index >= hdr->tagCount) {
      return 0;
    }
    auto offset = sizeof(TrackHotHeader) + hdr->tagsOffset + index * sizeof(std::uint32_t);
    if (_hotData.size() < offset + sizeof(std::uint32_t)) {
      return 0;
    }
    std::uint32_t result;
    std::memcpy(&result, utility::as<std::uint32_t>(hdr, offset), sizeof(std::uint32_t));
    return result;
  }

  std::string_view TrackView::hotGetString(std::uint16_t offset, std::uint16_t len) const
  {
    if (len == 0) { return {}; }
    std::size_t const start = sizeof(TrackHotHeader) + offset;
    if (start + len > _hotData.size()) {
      return {};
    }
    return {utility::as<char>(_hotData.data(), start), len};
  }

  std::string_view TrackView::coldUri() const
  {
    auto* hdr = coldHeader();
    if (!hdr) { return {}; }
    return coldGetString(hdr->uriOffset, hdr->uriLen);
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
    if (!_coldData || len == 0 || offset + len > _coldData->size()) {
      return {};
    }
    auto const* data = _coldData->data();
    return std::string_view{reinterpret_cast<char const*>(data + offset), len};
  }

  std::vector<std::pair<std::string, std::string>> TrackView::coldCustomMeta() const
  {
    std::vector<std::pair<std::string, std::string>> result;
    auto* hdr = coldHeader();
    if (!_coldData || !hdr || _coldData->size() < sizeof(TrackColdHeader)) {
      return result;
    }

    std::size_t offset = sizeof(TrackColdHeader);
    if (offset + sizeof(std::uint16_t) > _coldData->size()) {
      return result;
    }
    std::uint16_t pairCount = 0;
    std::memcpy(&pairCount, reinterpret_cast<std::byte const*>(hdr) + offset, sizeof(pairCount));
    offset += sizeof(pairCount);

    result.reserve(pairCount);
    auto const* data = _coldData->data();

    for (std::uint16_t i = 0; i < pairCount && offset < _coldData->size(); ++i) {
      if (offset + sizeof(std::uint16_t) * 2 > _coldData->size()) break;
      std::uint16_t keyLen = 0, valueLen = 0;
      std::memcpy(&keyLen, data + offset, sizeof(keyLen));
      offset += sizeof(keyLen);
      std::memcpy(&valueLen, data + offset, sizeof(valueLen));
      offset += sizeof(valueLen);

      if (offset + keyLen > _coldData->size()) break;
      std::string key(reinterpret_cast<char const*>(data + offset), keyLen);
      offset += keyLen;

      if (offset + valueLen > _coldData->size()) break;
      std::string value(reinterpret_cast<char const*>(data + offset), valueLen);
      offset += valueLen;

      result.emplace_back(std::move(key), std::move(value));
    }
    return result;
  }

  std::optional<std::string> TrackView::coldCustomValue(std::string_view key) const
  {
    auto* hdr = coldHeader();
    if (!_coldData || !hdr || _coldData->size() < sizeof(TrackColdHeader)) {
      return std::nullopt;
    }

    std::size_t offset = sizeof(TrackColdHeader);
    if (offset + sizeof(std::uint16_t) > _coldData->size()) {
      return std::nullopt;
    }
    std::uint16_t pairCount = 0;
    std::memcpy(&pairCount, reinterpret_cast<std::byte const*>(hdr) + offset, sizeof(pairCount));
    offset += sizeof(pairCount);
    auto const* data = _coldData->data();

    for (std::uint16_t i = 0; i < pairCount && offset < _coldData->size(); ++i) {
      if (offset + sizeof(std::uint16_t) * 2 > _coldData->size()) break;
      std::uint16_t keyLen = 0, valueLen = 0;
      std::memcpy(&keyLen, data + offset, sizeof(keyLen));
      offset += sizeof(keyLen);
      std::memcpy(&valueLen, data + offset, sizeof(valueLen));
      offset += sizeof(valueLen);

      if (offset + keyLen > _coldData->size()) break;
      std::string_view currentKey(reinterpret_cast<char const*>(data + offset), keyLen);
      offset += keyLen;

      if (currentKey == key) {
        if (offset + valueLen > _coldData->size()) break;
        std::string value(reinterpret_cast<char const*>(data + offset), valueLen);
        return value;
      }
      offset += valueLen;
    }
    return std::nullopt;
  }

  // TrackView proxy implementations
  std::vector<DictionaryId> TrackView::TagProxy::ids() const
  {
    std::vector<DictionaryId> result;
    auto c = count();
    for (std::uint8_t i = 0; i < c; ++i)
    {
      auto tagId = id(i);
      if (tagId > 0)
      {
        result.push_back(tagId);
      }
    }
    return result;
  }

  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const
  {
    auto c = count();
    for (std::uint8_t i = 0; i < c; ++i)
    {
      if (id(i) == tagIdToCheck)
      {
        return true;
      }
    }
    return false;
  }

  std::vector<std::pair<std::string, std::string>> TrackView::CustomProxy::all() const
  {
    return _track.coldCustomMeta();
  }

  std::optional<std::string> TrackView::CustomProxy::get(std::string_view key) const
  {
    return _track.coldCustomValue(key);
  }

  std::vector<std::byte> encodeColdData(
      TrackColdHeader const& header,
      std::vector<std::pair<std::string, std::string>> const& customMeta,
      std::string_view uri)
  {
    std::vector<std::byte> result;

    // Calculate custom meta size
    std::uint16_t customMetaSize = sizeof(std::uint16_t); // pairCount
    for (auto const& [key, value] : customMeta) {
      customMetaSize += sizeof(std::uint16_t) * 2; // keyLen + valueLen
      customMetaSize += static_cast<std::uint16_t>(key.size() + value.size());
    }

    // Calculate uri offset
    std::uint16_t varDataOffset = sizeof(TrackColdHeader);
    std::uint16_t uriOffset = varDataOffset + customMetaSize;
    std::uint16_t uriLen = static_cast<std::uint16_t>(uri.size());

    // Reserve space
    result.reserve(sizeof(TrackColdHeader) + customMetaSize + uriLen + 1);

    // Build header with correct offsets
    TrackColdHeader hdr = header;
    hdr.uriOffset = uriOffset;
    hdr.uriLen = uriLen;

    // Write fixed header
    auto const* headerBytes = reinterpret_cast<std::byte const*>(&hdr);
    result.insert(result.end(), headerBytes, headerBytes + sizeof(TrackColdHeader));

    // Write customPairCount
    std::byte buf[sizeof(std::uint16_t)];
    std::uint16_t pairCount = static_cast<std::uint16_t>(customMeta.size());
    std::memcpy(buf, &pairCount, sizeof(pairCount));
    result.insert(result.end(), buf, buf + sizeof(pairCount));

    // Write custom key-value pairs
    for (auto const& [key, value] : customMeta) {
      std::uint16_t keyLen = static_cast<std::uint16_t>(key.size());
      std::memcpy(buf, &keyLen, sizeof(keyLen));
      result.insert(result.end(), buf, buf + sizeof(keyLen));

      std::uint16_t valueLen = static_cast<std::uint16_t>(value.size());
      std::memcpy(buf, &valueLen, sizeof(valueLen));
      result.insert(result.end(), buf, buf + sizeof(valueLen));

      auto const* keyBytes = reinterpret_cast<std::byte const*>(key.data());
      result.insert(result.end(), keyBytes, keyBytes + key.size());

      auto const* valueBytes = reinterpret_cast<std::byte const*>(value.data());
      result.insert(result.end(), valueBytes, valueBytes + value.size());
    }

    // Write uri (null-terminated)
    if (!uri.empty()) {
      auto const* uriBytes = reinterpret_cast<std::byte const*>(uri.data());
      result.insert(result.end(), uriBytes, uriBytes + uri.size());
    }
    result.push_back(std::byte{'\0'});

    // Pad to 4-byte alignment
    while (result.size() % 4 != 0) {
      result.push_back(std::byte{0});
    }

    return result;
  }

  std::vector<std::byte> encodeColdCustomMeta(
      std::vector<std::pair<std::string, std::string>> const& customMeta)
  {
    std::vector<std::byte> result;

    // Reserve approximate space: 2 bytes for count + per-pair overhead
    result.reserve(2 + customMeta.size() * 4);

    // Write pairCount
    std::byte buf[sizeof(std::uint16_t)];
    std::uint16_t pairCount = static_cast<std::uint16_t>(customMeta.size());
    std::memcpy(buf, &pairCount, sizeof(pairCount));
    result.insert(result.end(), buf, buf + sizeof(pairCount));

    for (auto const& [key, value] : customMeta) {
      std::uint16_t keyLen = static_cast<std::uint16_t>(key.size());
      std::memcpy(buf, &keyLen, sizeof(keyLen));
      result.insert(result.end(), buf, buf + sizeof(keyLen));

      std::uint16_t valueLen = static_cast<std::uint16_t>(value.size());
      std::memcpy(buf, &valueLen, sizeof(valueLen));
      result.insert(result.end(), buf, buf + sizeof(valueLen));

      auto const* keyBytes = reinterpret_cast<std::byte const*>(key.data());
      result.insert(result.end(), keyBytes, keyBytes + key.size());

      auto const* valueBytes = reinterpret_cast<std::byte const*>(value.data());
      result.insert(result.end(), valueBytes, valueBytes + value.size());
    }

    // Pad to 4-byte alignment
    while (result.size() % 4 != 0) {
      result.push_back(std::byte{0});
    }

    return result;
  }

  std::string normalizeKey(std::string_view key)
  {
    std::string result;
    result.reserve(key.size());

    // Find first non-whitespace
    std::size_t start = 0;
    while (start < key.size() && std::isspace(static_cast<unsigned char>(key[start]))) {
      ++start;
    }

    // Find last non-whitespace
    std::size_t end = key.size();
    while (end > start && std::isspace(static_cast<unsigned char>(key[end - 1]))) {
      --end;
    }

    // Convert to lowercase
    for (std::size_t i = start; i < end; ++i) {
      result += static_cast<char>(std::tolower(static_cast<unsigned char>(key[i])));
    }

    return result;
  }

} // namespace rs::core
