// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace test
{

  /**
   * Serialize a POD struct to a byte vector.
   */
  template<typename T>
  std::vector<char> serializeHeader(T const& header)
  {
    static_assert(std::is_trivially_copyable_v<T>, "Header must be trivially copyable");

    std::vector<char> data;
    data.insert(data.end(), reinterpret_cast<char const*>(&header), reinterpret_cast<char const*>(&header + 1));
    return data;
  }

  /**
   * Append a null-terminated string to the payload.
   */
  inline void appendString(std::vector<char>& payload, std::string_view str)
  {
    payload.insert(payload.end(), str.begin(), str.end());
    payload.push_back('\0');
  }

} // namespace test
