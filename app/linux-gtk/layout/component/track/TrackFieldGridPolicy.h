// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::gtk::layout::track_field_grid
{
  struct SectionAvailability final
  {
    bool hasMetadataFields = false;
    bool hasSelectedTracks = false;
    bool hasTechnicalFields = false;
  };

  constexpr bool shouldRenderMetadataSection(SectionAvailability const availability)
  {
    return availability.hasMetadataFields;
  }

  constexpr bool shouldRenderCustomSection(SectionAvailability const availability)
  {
    return availability.hasSelectedTracks;
  }

  constexpr bool shouldRenderTechnicalSection(SectionAvailability const availability)
  {
    return availability.hasTechnicalFields;
  }

  struct MetadataFieldVisibility final
  {
    bool metadataExpanded = false;
    bool showEmptyMetadata = false;
    bool editorEditing = false;
    bool hasDisplayText = false;
  };

  constexpr bool shouldShowMetadataFieldRow(MetadataFieldVisibility const visibility)
  {
    return visibility.metadataExpanded &&
           (visibility.showEmptyMetadata || visibility.editorEditing || visibility.hasDisplayText);
  }

  struct CompositeMetadataFieldVisibility final
  {
    bool metadataExpanded = false;
    bool showEmptyMetadata = false;
    bool primaryEditorEditing = false;
    bool secondaryEditorEditing = false;
    bool hasPrimaryDisplayText = false;
    bool hasSecondaryDisplayText = false;
  };

  constexpr bool shouldShowCompositeMetadataRow(CompositeMetadataFieldVisibility const visibility)
  {
    return visibility.metadataExpanded &&
           (visibility.showEmptyMetadata || visibility.primaryEditorEditing || visibility.secondaryEditorEditing ||
            visibility.hasPrimaryDisplayText || visibility.hasSecondaryDisplayText);
  }
} // namespace ao::gtk::layout::track_field_grid
