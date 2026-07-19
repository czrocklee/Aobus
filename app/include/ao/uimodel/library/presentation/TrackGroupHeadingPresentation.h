// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <string>

namespace ao::uimodel
{
  struct TrackGroupHeadingPresentation final
  {
    std::string primaryText{};
    std::string secondaryText{};
    std::string tertiaryText{};

    bool operator==(TrackGroupHeadingPresentation const&) const = default;
  };

  TrackGroupHeadingPresentation formatTrackGroupHeading(PresentationTextCatalog const& textCatalog,
                                                        rt::TrackGroupHeading const& heading);
} // namespace ao::uimodel
