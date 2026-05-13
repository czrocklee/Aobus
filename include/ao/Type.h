// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/TaggedInteger.h>

#include <cstdint>

namespace ao
{
  using TrackId = ao::utility::TaggedInteger<std::uint32_t, struct TrackIdTag>;
  using ListId = ao::utility::TaggedInteger<std::uint32_t, struct ListIdTag>;
  using ResourceId = ao::utility::TaggedInteger<std::uint32_t, struct ResourceIdTag>;
  using DictionaryId = ao::utility::TaggedInteger<std::uint32_t, struct DictionaryIdTag>;
} // namespace ao
