// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackView.h>

#include <algorithm>
#include <cstring>

namespace rs::core
{

  // TrackHotView::HotTagProxy implementation
  std::vector<DictionaryId> TrackHotView::HotTagProxy::ids() const
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

  bool TrackHotView::HotTagProxy::has(DictionaryId tagIdToCheck) const
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

  std::string_view TrackHotView::getString(std::uint16_t offset, std::uint16_t len) const
  {
    if (len == 0) { return {}; };

    std::size_t const start = sizeof(TrackHotHeader) + offset;
    if (start + len > _size)
    {
      return {};
    }

    return {utility::as<char>(_header, start), len};
  }

  std::uint32_t TrackHotView::tagId(std::uint8_t index) const
  {
    if (index >= _header->tagCount)
    {
      return 0;
    }
    auto offset = sizeof(TrackHotHeader) + _header->tagsOffset + index * sizeof(std::uint32_t);
    if (offset + sizeof(std::uint32_t) > _size)
    {
      return 0;
    }
    return *utility::as<std::uint32_t>(_header, offset);
  }

  // TrackColdView implementation
  std::string_view TrackColdView::uri() const
  {
    return getString(_header->uriOffset, _header->uriLen);
  }

  std::string_view TrackColdView::getString(std::uint16_t offset, std::uint16_t len) const
  {
    if (len == 0 || offset + len > _size) {
      return {};
    }
    auto const* data = reinterpret_cast<std::byte const*>(_header);
    return std::string_view{reinterpret_cast<char const*>(data + offset), len};
  }

  std::vector<std::pair<std::string, std::string>> TrackColdView::customMeta() const
  {
    std::vector<std::pair<std::string, std::string>> result;

    if (isNull() || _size < sizeof(TrackColdHeader)) {
      return result;
    }

    // Custom meta starts right after the fixed header
    std::size_t offset = sizeof(TrackColdHeader);

    // Read customPairCount
    if (offset + sizeof(std::uint16_t) > _size) {
      return result;
    }
    std::uint16_t pairCount = 0;
    std::memcpy(&pairCount, reinterpret_cast<std::byte const*>(_header) + offset, sizeof(pairCount));
    offset += sizeof(pairCount);

    result.reserve(pairCount);

    for (std::uint16_t i = 0; i < pairCount && offset < _size; ++i) {
      // Read keyLen
      if (offset + sizeof(std::uint16_t) > _size) break;
      std::uint16_t keyLen = 0;
      std::memcpy(&keyLen, reinterpret_cast<std::byte const*>(_header) + offset, sizeof(keyLen));
      offset += sizeof(keyLen);

      // Read valueLen
      if (offset + sizeof(std::uint16_t) > _size) break;
      std::uint16_t valueLen = 0;
      std::memcpy(&valueLen, reinterpret_cast<std::byte const*>(_header) + offset, sizeof(valueLen));
      offset += sizeof(valueLen);

      // Read key bytes
      if (offset + keyLen > _size) break;
      std::string key(reinterpret_cast<char const*>(reinterpret_cast<std::byte const*>(_header) + offset), keyLen);
      offset += keyLen;

      // Read value bytes
      if (offset + valueLen > _size) break;
      std::string value(reinterpret_cast<char const*>(reinterpret_cast<std::byte const*>(_header) + offset), valueLen);
      offset += valueLen;

      result.emplace_back(std::move(key), std::move(value));
    }

    return result;
  }

  std::optional<std::string> TrackColdView::customValue(std::string_view key) const
  {
    if (isNull() || _size < sizeof(TrackColdHeader)) {
      return std::nullopt;
    }

    std::size_t offset = sizeof(TrackColdHeader);

    // Read customPairCount
    if (offset + sizeof(std::uint16_t) > _size) {
      return std::nullopt;
    }
    std::uint16_t pairCount = 0;
    std::memcpy(&pairCount, reinterpret_cast<std::byte const*>(_header) + offset, sizeof(pairCount));
    offset += sizeof(pairCount);

    for (std::uint16_t i = 0; i < pairCount && offset < _size; ++i) {
      // Read keyLen
      if (offset + sizeof(std::uint16_t) > _size) break;
      std::uint16_t keyLen = 0;
      std::memcpy(&keyLen, reinterpret_cast<std::byte const*>(_header) + offset, sizeof(keyLen));
      offset += sizeof(keyLen);

      // Read valueLen
      if (offset + sizeof(std::uint16_t) > _size) break;
      std::uint16_t valueLen = 0;
      std::memcpy(&valueLen, reinterpret_cast<std::byte const*>(_header) + offset, sizeof(valueLen));
      offset += sizeof(valueLen);

      // Read and compare key
      if (offset + keyLen > _size) break;
      std::string_view currentKey(reinterpret_cast<char const*>(reinterpret_cast<std::byte const*>(_header) + offset), keyLen);
      offset += keyLen;

      if (currentKey == key) {
        // Read value
        if (offset + valueLen > _size) break;
        std::string value(reinterpret_cast<char const*>(reinterpret_cast<std::byte const*>(_header) + offset), valueLen);
        return value;
      }

      // Skip value
      offset += valueLen;
    }

    return std::nullopt;
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
