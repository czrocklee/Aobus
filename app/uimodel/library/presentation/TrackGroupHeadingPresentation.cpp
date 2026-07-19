// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <concepts>
#include <cstdint>
#include <string>
#include <variant>

namespace ao::uimodel
{
  namespace
  {
    std::string presentHeadingValue(PresentationTextCatalog const& textCatalog, rt::TrackGroupHeadingValue const& value)
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
            return std::string{textCatalog.missingTrackValueLabel(item)};
          }
        },
        value);
    }
  } // namespace

  TrackGroupHeadingPresentation formatTrackGroupHeading(PresentationTextCatalog const& textCatalog,
                                                        rt::TrackGroupHeading const& heading)
  {
    return TrackGroupHeadingPresentation{
      .primaryText = presentHeadingValue(textCatalog, heading.primary),
      .secondaryText = presentHeadingValue(textCatalog, heading.secondary),
      .tertiaryText = presentHeadingValue(textCatalog, heading.tertiary),
    };
  }
} // namespace ao::uimodel
