// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/ListBuilder.h>

#include <rs/utility/ByteView.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace rs::core
{

  //=============================================================================
  // ListBuilder - factory methods
  //=============================================================================

  ListBuilder ListBuilder::createNew()
  {
    return ListBuilder{ListRecord{}};
  }

  ListBuilder ListBuilder::fromRecord(ListRecord record)
  {
    return ListBuilder{std::move(record)};
  }

  ListBuilder ListBuilder::fromView(ListView const& view)
  {
    auto record = ListRecord{};
    record.name = std::string{view.name()};
    record.description = std::string{view.description()};
    record.filter = std::string{view.filter()};

    auto tracks = view.tracks();
    record.trackIds.reserve(tracks.size());
    for (auto const& id : tracks) { record.trackIds.push_back(id); }

    return ListBuilder{std::move(record)};
  }

  ListBuilder::ListBuilder(ListRecord record)
    : _record{std::move(record)}
  {
  }

  ListBuilder::TracksBuilder ListBuilder::tracks()
  {
    return TracksBuilder{*this};
  }

  ListBuilder& ListBuilder::name(std::string v)
  {
    _record.name = std::move(v);
    return *this;
  }

  ListBuilder& ListBuilder::description(std::string v)
  {
    _record.description = std::move(v);
    return *this;
  }

  ListBuilder& ListBuilder::filter(std::string v)
  {
    _record.filter = std::move(v);
    return *this;
  }

  //=============================================================================
  // TracksBuilder
  //=============================================================================

  ListBuilder::TracksBuilder& ListBuilder::TracksBuilder::add(TrackId id)
  {
    _builder._record.trackIds.push_back(id);
    return *this;
  }

  ListBuilder::TracksBuilder& ListBuilder::TracksBuilder::remove(TrackId id)
  {
    std::erase(_builder._record.trackIds, id);
    return *this;
  }

  //=============================================================================
  // ListBuilder - serialization
  //=============================================================================

  std::vector<std::byte> ListBuilder::serialize() const
  {
    auto const& name = _record.name;
    auto const& description = _record.description;
    auto const& expression = _record.filter;
    auto const& trackIds = _record.trackIds;

    auto const nameLen = name.size();
    auto const descLen = description.size();
    auto const filterLen = expression.size();
    auto const trackIdsSize = trackIds.size() * sizeof(TrackId);

    // Offsets are relative to kListHeaderSize (start of trackIds array)
    // No internal alignment, just pack fields consecutively
    auto const descOffset = trackIdsSize + nameLen;
    auto const filterOffset = descOffset + descLen;

    // Total payload size, aligned to 4 bytes for LMDB
    auto const totalSize = kListHeaderSize + trackIdsSize + nameLen + descLen + filterLen;
    auto const payloadSize = (totalSize + 3) & ~3ULL;

    auto result = std::vector<std::byte>{};
    result.reserve(payloadSize);

    // Build header
    auto header = ListHeader{};
    header.trackIdsCount = static_cast<std::uint32_t>(trackIds.size());
    header.nameOffset = static_cast<std::uint16_t>(trackIdsSize);
    header.nameLen = static_cast<std::uint16_t>(nameLen);
    header.descOffset = static_cast<std::uint16_t>(descOffset);
    header.descLen = static_cast<std::uint16_t>(descLen);
    header.filterOffset = static_cast<std::uint16_t>(filterOffset);
    header.filterLen = static_cast<std::uint16_t>(filterLen);

    // Copy header
    result.insert_range(result.end(), utility::asBytes(header));

    // Copy trackIds
    if (!trackIds.empty()) { result.insert_range(result.end(), utility::asBytes(trackIds.data(), trackIds.size())); }

    // Copy strings
    result.insert_range(result.end(), utility::asBytes(name));
    result.insert_range(result.end(), utility::asBytes(description));
    result.insert_range(result.end(), utility::asBytes(expression));

    // Pad to 4-byte alignment
    while (result.size() % 4 != 0) { result.push_back(std::byte{0}); }

    return result;
  }

} // namespace rs::core
