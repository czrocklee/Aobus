// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

// POSITIVE
[[nodiscard]] std::int32_t getForbiddenVal()
{
  return 42;
}

// POSITIVE
struct [[nodiscard]] ForbiddenStruct
{};

// POSITIVE
class [[nodiscard]] ForbiddenClass
{};

// NEGATIVE
std::int32_t getConformingVal()
{
  return 42;
}
