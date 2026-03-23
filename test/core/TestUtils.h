// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/utility/ByteView.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace test
{
  /**
   * Serialize a POD struct to a byte vector.
   */
  template<typename T>
  std::vector<std::byte> serializeHeader(T const& header)
  {
    static_assert(std::is_trivially_copyable_v<T>, "Header must be trivially copyable");

    std::vector<std::byte> data;
    data.insert_range(data.end(), rs::utility::asBytes(header));
    return data;
  }

  /**
   * Append a null-terminated string to the payload.
   */
  inline void appendString(std::vector<std::byte>& payload, std::string_view str)
  {
    payload.insert_range(payload.end(), rs::utility::asBytes(str));
    payload.push_back(std::byte{'\0'});
  }

} // namespace test
