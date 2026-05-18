// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <cstdint>

namespace ao
{
  using TrackId = utility::StrongType<std::uint32_t, struct TrackIdTag>;
  using ListId = utility::StrongType<std::uint32_t, struct ListIdTag>;
  using ResourceId = utility::StrongType<std::uint32_t, struct ResourceIdTag>;
  using DictionaryId = utility::StrongType<std::uint32_t, struct DictionaryIdTag>;
} // namespace ao
