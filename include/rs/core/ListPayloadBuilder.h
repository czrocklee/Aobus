// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/ListLayout.h>
#include <rs/core/Type.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rs::core
{

/**
 * ListPayloadBuilder - Builds binary list payloads for ListStore.
 *
 * Constructs a ListLayout binary payload from name, description,
 * filter expression, and optional TrackId list.
 */
class ListPayloadBuilder
{
public:
  /**
   * Build a smart list payload.
   * @param name List name
   * @param description List description
   * @param expression Filter expression (stored directly in payload)
   * @return Binary payload suitable for ListStore::Writer::create()
   */
  static std::vector<std::byte> buildSmartList(
      std::string_view name,
      std::string_view description,
      std::string_view expression);

  /**
   * Build a manual list payload.
   * @param name List name
   * @param description List description
   * @param trackIds List of track IDs
   * @return Binary payload suitable for ListStore::Writer::create()
   */
  static std::vector<std::byte> buildManualList(
      std::string_view name,
      std::string_view description,
      std::span<TrackId const> trackIds);

  /**
   * Build a list payload.
   * @param name List name
   * @param description List description
   * @param expression Filter expression (empty for manual lists)
   * @param trackIds Track IDs for manual list (empty for smart list)
   * @return Binary payload suitable for ListStore::Writer::create()
   */
  static std::vector<std::byte> build(
      std::string_view name,
      std::string_view description,
      std::string_view expression,
      std::span<TrackId const> trackIds);

private:
  static constexpr std::size_t kHeaderSize = sizeof(ListHeader);
};

} // namespace rs::core