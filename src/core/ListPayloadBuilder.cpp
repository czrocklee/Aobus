// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/ListPayloadBuilder.h>

#include <rs/utility/ByteView.h>

#include <cstring>
#include <vector>

namespace rs::core
{

std::vector<std::byte> ListPayloadBuilder::buildSmartList(
    std::string_view name,
    std::string_view description,
    std::string_view expression)
{
  return build(name, description, expression, {});
}

std::vector<std::byte> ListPayloadBuilder::buildManualList(
    std::string_view name,
    std::string_view description,
    std::span<TrackId const> trackIds)
{
  return build(name, description, {}, trackIds);
}

std::vector<std::byte> ListPayloadBuilder::build(
    std::string_view name,
    std::string_view description,
    std::string_view expression,
    std::span<TrackId const> trackIds)
{
  auto const nameLen = name.size();
  auto const descLen = description.size();
  auto const filterLen = expression.size();
  auto const trackIdsSize = trackIds.size() * sizeof(TrackId);

  // Layout: [header][trackIds][name][description][filter]
  // name follows trackIds, aligned to 4 bytes
  auto const nameOffset = static_cast<std::uint64_t>(trackIdsSize);
  auto const alignedNameOffset = (nameOffset + 3) & ~3ULL;

  // description follows name
  auto const descOffset = alignedNameOffset + nameLen;
  auto const alignedDescOffset = (descOffset + 3) & ~3ULL;

  // filter follows description
  auto const filterOffset = alignedDescOffset + descLen;
  auto const alignedFilterOffset = (filterOffset + 3) & ~3ULL;

  // Total payload size
  auto const totalSize = kHeaderSize + alignedFilterOffset + filterLen;

  std::vector<std::byte> payload(totalSize);

  // Build header
  ListHeader header = {};
  header.trackIdsCount = static_cast<std::uint32_t>(trackIds.size());
  header.nameOffset = static_cast<std::uint16_t>(alignedNameOffset);
  header.nameLen = static_cast<std::uint16_t>(nameLen);
  header.descOffset = static_cast<std::uint16_t>(alignedDescOffset);
  header.descLen = static_cast<std::uint16_t>(descLen);
  header.filterOffset = static_cast<std::uint16_t>(alignedFilterOffset);
  header.filterLen = static_cast<std::uint16_t>(filterLen);

  // Copy header to payload
  std::memcpy(payload.data(), &header, sizeof(header));

  // Copy track IDs to payload (if any)
  if (!trackIds.empty()) {
    auto const trackIdsStart = payload.data() + kHeaderSize;
    std::memcpy(trackIdsStart, trackIds.data(), trackIdsSize);
  }

  // Copy name string
  if (!name.empty()) {
    auto const nameStart = payload.data() + kHeaderSize + alignedNameOffset;
    std::memcpy(nameStart, name.data(), nameLen);
  }

  // Copy description string
  if (!description.empty()) {
    auto const descStart = payload.data() + kHeaderSize + alignedDescOffset;
    std::memcpy(descStart, description.data(), descLen);
  }

  // Copy filter expression string
  if (!expression.empty()) {
    auto const filterStart = payload.data() + kHeaderSize + alignedFilterOffset;
    std::memcpy(filterStart, expression.data(), filterLen);
  }

  return payload;
}

} // namespace rs::core
