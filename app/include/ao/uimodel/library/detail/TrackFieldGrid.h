// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  struct TrackFieldGridSchemaOptions final
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

  TrackFieldGridSchema buildTrackFieldGridSchema(TrackFieldGridSchemaOptions options = {});

  std::string formatMetadataHeader(std::string_view titleText, std::string_view artistText);
  std::string formatTechnicalHeader(std::string_view codecText,
                                    std::string_view sampleRateText,
                                    std::string_view bitDepthText);

  struct TrackFieldGridSectionAvailability final
  {
    bool metadataCategoryEnabled = true;
    bool hasMetadataFields = false;
    bool hasSelectedTracks = false;
    bool hasTechnicalFields = false;
  };

  constexpr bool shouldRenderMetadataSection(TrackFieldGridSectionAvailability const availability)
  {
    return availability.metadataCategoryEnabled && (availability.hasMetadataFields || availability.hasSelectedTracks);
  }

  constexpr bool shouldRenderCustomMetadataArea(TrackFieldGridSectionAvailability const availability)
  {
    return availability.metadataCategoryEnabled && availability.hasSelectedTracks;
  }

  constexpr bool shouldRenderTechnicalSection(TrackFieldGridSectionAvailability const availability)
  {
    return availability.hasTechnicalFields;
  }

  struct TrackFieldGridMetadataFieldVisibility final
  {
    bool metadataExpanded = false;
    bool showEmptyMetadata = false;
    bool editorEditing = false;
    bool hasDisplayText = false;
  };

  constexpr bool shouldShowTrackFieldGridMetadataFieldRow(TrackFieldGridMetadataFieldVisibility const visibility)
  {
    return visibility.metadataExpanded &&
           (visibility.showEmptyMetadata || visibility.editorEditing || visibility.hasDisplayText);
  }

  struct CompositeMetadataVisibility final
  {
    bool metadataExpanded = false;
    bool showEmptyMetadata = false;
    bool primaryEditorEditing = false;
    bool secondaryEditorEditing = false;
    bool hasPrimaryDisplayText = false;
    bool hasSecondaryDisplayText = false;
  };

  constexpr bool shouldShowCompositeMetadataRow(CompositeMetadataVisibility const visibility)
  {
    return visibility.metadataExpanded &&
           (visibility.showEmptyMetadata || visibility.primaryEditorEditing || visibility.secondaryEditorEditing ||
            visibility.hasPrimaryDisplayText || visibility.hasSecondaryDisplayText);
  }
} // namespace ao::uimodel
