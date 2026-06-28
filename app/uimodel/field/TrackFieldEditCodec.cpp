// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    std::string_view trimAsciiWhitespace(std::string_view value)
    {
      auto const first = value.find_first_not_of(" \t\n\r\f\v");

      if (first == std::string_view::npos)
      {
        return {};
      }

      auto const last = value.find_last_not_of(" \t\n\r\f\v");
      return value.substr(first, last - first + 1);
    }
  } // namespace

  TrackFieldEditValue makeTextEditValue(std::string_view value)
  {
    return TrackFieldEditValue{std::in_place_type<std::string>, std::string{value}};
  }

  Result<TrackFieldEditValue> parseTextEditValue(std::string_view value)
  {
    return makeTextEditValue(value);
  }

  Result<TrackFieldEditValue> parseUint16EditValue(std::string_view value)
  {
    auto const trimmed = trimAsciiWhitespace(value);

    if (trimmed.empty())
    {
      return TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(0)};
    }

    std::uint32_t parsed = 0;
    auto const* const begin = trimmed.data();
    auto const* const end = trimmed.data() + trimmed.size();
    auto const [ptr, ec] = std::from_chars(begin, end, parsed);

    if (ec != std::errc{} || ptr != end || parsed > std::numeric_limits<std::uint16_t>::max())
    {
      return makeError(Error::Code::FormatRejected, "Enter a whole number from 0 to 65535.");
    }

    return TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(parsed)};
  }
} // namespace ao::uimodel
