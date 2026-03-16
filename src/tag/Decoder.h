// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/tag/ValueType.h>

#include <charconv>

namespace rs::tag
{
  struct StringDecoder
  {
    static ValueType decode(void const* buffer, std::size_t size)
    {
      return std::string{static_cast<char const*>(buffer), size};
    }
  };

  struct IntDecoder
  {
    static ValueType decode(void const* buffer, std::size_t size)
    {
      char const* data = static_cast<char const*>(buffer);
      std::int64_t result;
      auto [_, ec] = std::from_chars(data, data + size, result);
      return ec == std::errc() ? ValueType{result} : ValueType{};
    }
  };

  struct BlobDecoder
  {
    static ValueType decode(void const* buffer, std::size_t size)
    {
      char const* data = static_cast<char const*>(buffer);
      std::vector<char> blob;
      blob.assign(data, data + size);
      return {std::move(blob)};
    }
  };
}