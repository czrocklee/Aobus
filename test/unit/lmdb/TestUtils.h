// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace ao::lmdb::test
{
  using ao::test::TempDir;

  /**
   * Create a vector filled with test data.
   */
  inline std::vector<std::byte> createTestData(std::size_t size)
  {
    auto data = std::vector<std::byte>(size);

    for (std::size_t i = 0; i < size; ++i)
    {
      data[i] = static_cast<std::byte>(i % 256);
    }

    return data;
  }

  /**
   * Simple string data for testing.
   */
  inline std::vector<std::byte> createStringData(std::string_view str)
  {
    auto bytes = utility::bytes::view(str);
    return {bytes.begin(), bytes.end()};
  }
} // namespace ao::lmdb::test
