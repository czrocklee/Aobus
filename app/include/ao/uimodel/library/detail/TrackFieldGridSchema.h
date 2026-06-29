// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  struct TrackFieldGridSchemaRequest final
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

  struct TrackFieldGridSchema final
  {
    std::vector<rt::TrackField> metadataFields;
    std::vector<TrackFieldGridCompositeFields> compositeMetadataFields;
    std::vector<rt::TrackField> technicalFields;
  };

  TrackFieldGridSchema buildTrackFieldGridSchema(TrackFieldGridSchemaRequest request);

  std::string formatMetadataHeader(std::string_view titleText, std::string_view artistText);
  std::string formatTechnicalHeader(std::string_view codecText,
                                    std::string_view sampleRateText,
                                    std::string_view bitDepthText);
} // namespace ao::uimodel
