// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/projection/TrackListProjection.h>

#include <optional>
#include <string>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  struct TrackGroupHeadingPresentation final
  {
    std::string primaryText{};
    std::string secondaryText{};
    std::string tertiaryText{};

    bool operator==(TrackGroupHeadingPresentation const&) const = default;
  };

  TrackGroupHeadingPresentation formatTrackGroupHeading(i18n::MessageCatalog const& textCatalog,
                                                        rt::TrackGroupHeading const& heading);
  std::optional<std::string> trackGroupCoverArtMonogram(rt::TrackGroupHeading const& heading);
} // namespace ao::uimodel
