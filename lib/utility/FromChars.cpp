// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/FromChars.h>

#include <fast_float/fast_float.h>

#include <charconv>
#include <system_error>

namespace ao::utility
{
  namespace
  {
    template<typename T>
    std::from_chars_result parseFloat(char const* first, char const* last, T& value)
    {
      T parsed = 0;
      auto const result = fast_float::from_chars(first, last, parsed);

      if (result.ec == std::errc{})
      {
        value = parsed;
      }

      return {result.ptr, result.ec};
    }
  } // namespace

  std::from_chars_result fromChars(char const* first, char const* last, float& value)
  {
    return parseFloat(first, last, value);
  }

  std::from_chars_result fromChars(char const* first, char const* last, double& value)
  {
    return parseFloat(first, last, value);
  }
} // namespace ao::utility
