// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <array>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace ao::uimodel
{
  namespace
  {
    std::string presentHeadingValue(i18n::MessageCatalog const& textCatalog, rt::TrackGroupHeadingValue const& value)
    {
      return std::visit(
        [&textCatalog]<typename Value>(Value const& item) -> std::string
        {
          if constexpr (std::same_as<Value, std::monostate>)
          {
            return {};
          }
          else if constexpr (std::same_as<Value, std::string>)
          {
            return item;
          }
          else if constexpr (std::same_as<Value, std::uint16_t>)
          {
            return std::to_string(item);
          }
          else
          {
            return std::string{missingTrackValueLabel(textCatalog, item)};
          }
        },
        value);
    }
  } // namespace

  TrackGroupHeadingPresentation formatTrackGroupHeading(i18n::MessageCatalog const& textCatalog,
                                                        rt::TrackGroupHeading const& heading)
  {
    return TrackGroupHeadingPresentation{
      .primaryText = presentHeadingValue(textCatalog, heading.primary),
      .secondaryText = presentHeadingValue(textCatalog, heading.secondary),
      .tertiaryText = presentHeadingValue(textCatalog, heading.tertiary),
    };
  }

  std::optional<std::string> trackGroupCoverArtMonogram(rt::TrackGroupHeading const& heading)
  {
    return std::visit(
      []<typename Value>(Value const& item) -> std::optional<std::string>
      {
        if constexpr (std::same_as<Value, std::uint16_t>)
        {
          auto buffer = std::array<char, std::numeric_limits<std::uint16_t>::digits10 + 1>{};
          auto const result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), item);
          auto const begin = result.ptr - buffer.data() > 2 ? result.ptr - 2 : buffer.data();
          return std::string{begin, result.ptr};
        }
        else if constexpr (std::same_as<Value, rt::MissingTrackValueKind>)
        {
          return std::string{"?"};
        }
        else
        {
          return std::nullopt;
        }
      },
      heading.primary);
  }
} // namespace ao::uimodel
