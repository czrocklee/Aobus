// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::track
{
  struct TrackFieldGridProjectionRequest final
  {
    bool includeMetadata = true;
    bool includeTechnical = true;
  };

  struct TrackFieldGridCompositeFields final
  {
    rt::TrackField primaryField;
    rt::TrackField secondaryField;

    bool operator==(TrackFieldGridCompositeFields const&) const = default;
  };

  struct TrackFieldGridProjection final
  {
    std::vector<rt::TrackField> metadataFields;
    std::vector<TrackFieldGridCompositeFields> compositeMetadataFields;
    std::vector<rt::TrackField> technicalFields;
  };

  TrackFieldGridProjection projectTrackFieldGrid(TrackFieldGridProjectionRequest request);

  std::string formatTrackFieldGridMetadataHeader(std::string_view titleText, std::string_view artistText);
  std::string formatTrackFieldGridTechnicalHeader(std::string_view codecText,
                                                  std::string_view sampleRateText,
                                                  std::string_view bitDepthText);
} // namespace ao::uimodel::track
