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
    return ListBuilder{};
  }

  ListBuilder ListBuilder::fromRecord(ListRecord const& record)
  {
    auto builder = ListBuilder{};
    builder._parentId = record.parentId;
    builder._name = record.name;
    builder._description = record.description;
    builder._filter = record.filter;
    builder._tracksBuilder._trackIds = record.trackIds;
    builder._tracksBuilder._isSmart = !record.filter.empty();
    return builder;
  }

  ListBuilder ListBuilder::fromView(ListView const& view)
  {
    auto builder = ListBuilder{};
    builder._parentId = view.parentId();
    builder._name = view.name();
    builder._description = view.description();
    builder._filter = view.filter();
    builder._tracksBuilder._isSmart = view.isSmart();

    builder._tracksBuilder._trackIds.append_range(view.tracks());
    return builder;
  }

  ListBuilder::TracksBuilder& ListBuilder::tracks()
  {
    return _tracksBuilder;
  }

  //=============================================================================
  // ListBuilder::record() - constructs ListRecord on-the-fly
  //=============================================================================

  ListRecord ListBuilder::record() const
  {
    auto record = ListRecord{};
    record.parentId = _parentId;
    record.name = std::string{_name};
    record.description = std::string{_description};
    record.filter = std::string{_filter};
    record.trackIds = _tracksBuilder._trackIds;
    return record;
  }

  //=============================================================================
  // Direct setters
  //=============================================================================

  ListBuilder& ListBuilder::name(std::string_view v)
  {
    _name = v;
    return *this;
  }

  ListBuilder& ListBuilder::description(std::string_view v)
  {
    _description = v;
    return *this;
  }

  ListBuilder& ListBuilder::filter(std::string_view v)
  {
    _filter = v;
    return *this;
  }

  ListBuilder& ListBuilder::parentId(ListId v)
  {
    _parentId = v;
    return *this;
  }

  //=============================================================================
  // TracksBuilder
  //=============================================================================

  ListBuilder::TracksBuilder& ListBuilder::TracksBuilder::add(TrackId id)
  {
    _trackIds.push_back(id);
    return *this;
  }

  ListBuilder::TracksBuilder& ListBuilder::TracksBuilder::remove(TrackId id)
  {
    std::erase(_trackIds, id);
    return *this;
  }

  ListBuilder::TracksBuilder& ListBuilder::TracksBuilder::clear()
  {
    _trackIds.clear();
    return *this;
  }

  ListBuilder::TracksBuilder& ListBuilder::TracksBuilder::isSmart(bool v)
  {
    _isSmart = v;
    return *this;
  }

  //=============================================================================
  // ListBuilder - serialization
  //=============================================================================

  std::vector<std::byte> ListBuilder::serialize() const
  {
    auto const& name = _name;
    auto const& description = _description;
    auto const& expression = _filter;
    auto const& trackIds = _tracksBuilder._trackIds;

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
    header.parentId = _parentId.value();

    // Copy header
    result.insert_range(result.end(), utility::bytes::view(header));

    // Copy trackIds
    if (!trackIds.empty())
    {
      result.insert_range(result.end(), utility::bytes::view(std::span<TrackId const>{trackIds}));
    }

    // Copy strings
    result.insert_range(result.end(), utility::bytes::view(name));
    result.insert_range(result.end(), utility::bytes::view(description));
    result.insert_range(result.end(), utility::bytes::view(expression));

    // Pad to 4-byte alignment
    while (result.size() % 4 != 0)
    {
      result.push_back(std::byte{0});
    }

    return result;
  }

} // namespace rs::core
