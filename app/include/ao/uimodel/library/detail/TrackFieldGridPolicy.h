// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::uimodel
{
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
