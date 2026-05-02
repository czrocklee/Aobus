// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/utility/TaggedInteger.h>

#include <cstdint>

namespace ao
{
  using TrackId = utility::TaggedInteger<std::uint32_t, struct TrackIdTag>;
  using ListId = utility::TaggedInteger<std::uint32_t, struct ListIdTag>;
  using ResourceId = utility::TaggedInteger<std::uint32_t, struct ResourceIdTag>;
  using DictionaryId = utility::TaggedInteger<std::uint32_t, struct DictionaryIdTag>;
} // namespace ao
