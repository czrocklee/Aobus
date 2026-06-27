// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/track/TrackFieldGridPolicy.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::layout::track_field_grid::test
{
  TEST_CASE("TrackFieldGridPolicy renders sections from available rows and selection state",
            "[gtk][unit][trackfieldgrid][policy]")
  {
    CHECK_FALSE(shouldRenderMetadataSection(
      SectionAvailability{.hasMetadataFields = false, .hasSelectedTracks = true, .hasTechnicalFields = true}));
    CHECK(shouldRenderMetadataSection(
      SectionAvailability{.hasMetadataFields = true, .hasSelectedTracks = false, .hasTechnicalFields = false}));

    CHECK_FALSE(shouldRenderCustomSection(
      SectionAvailability{.hasMetadataFields = true, .hasSelectedTracks = false, .hasTechnicalFields = true}));
    CHECK(shouldRenderCustomSection(
      SectionAvailability{.hasMetadataFields = false, .hasSelectedTracks = true, .hasTechnicalFields = false}));

    CHECK_FALSE(shouldRenderTechnicalSection(
      SectionAvailability{.hasMetadataFields = true, .hasSelectedTracks = true, .hasTechnicalFields = false}));
    CHECK(shouldRenderTechnicalSection(
      SectionAvailability{.hasMetadataFields = false, .hasSelectedTracks = false, .hasTechnicalFields = true}));
  }

  TEST_CASE("TrackFieldGridPolicy shows metadata rows from expansion and content state",
            "[gtk][unit][trackfieldgrid][policy]")
  {
    CHECK_FALSE(shouldShowMetadataFieldRow(MetadataFieldVisibility{
      .metadataExpanded = false, .showEmptyMetadata = true, .editorEditing = true, .hasDisplayText = true}));

    CHECK_FALSE(shouldShowMetadataFieldRow(MetadataFieldVisibility{
      .metadataExpanded = true, .showEmptyMetadata = false, .editorEditing = false, .hasDisplayText = false}));

    CHECK(shouldShowMetadataFieldRow(MetadataFieldVisibility{
      .metadataExpanded = true, .showEmptyMetadata = false, .editorEditing = false, .hasDisplayText = true}));

    CHECK(shouldShowMetadataFieldRow(MetadataFieldVisibility{
      .metadataExpanded = true, .showEmptyMetadata = true, .editorEditing = false, .hasDisplayText = false}));

    CHECK(shouldShowMetadataFieldRow(MetadataFieldVisibility{
      .metadataExpanded = true, .showEmptyMetadata = false, .editorEditing = true, .hasDisplayText = false}));
  }

  TEST_CASE("TrackFieldGridPolicy shows composite metadata rows from either side",
            "[gtk][unit][trackfieldgrid][policy]")
  {
    CHECK_FALSE(shouldShowCompositeMetadataRow(CompositeMetadataFieldVisibility{.metadataExpanded = false,
                                                                                .showEmptyMetadata = true,
                                                                                .primaryEditorEditing = true,
                                                                                .secondaryEditorEditing = true,
                                                                                .hasPrimaryDisplayText = true,
                                                                                .hasSecondaryDisplayText = true}));

    CHECK_FALSE(shouldShowCompositeMetadataRow(CompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                                                .showEmptyMetadata = false,
                                                                                .primaryEditorEditing = false,
                                                                                .secondaryEditorEditing = false,
                                                                                .hasPrimaryDisplayText = false,
                                                                                .hasSecondaryDisplayText = false}));

    CHECK(shouldShowCompositeMetadataRow(CompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                                          .showEmptyMetadata = false,
                                                                          .primaryEditorEditing = false,
                                                                          .secondaryEditorEditing = false,
                                                                          .hasPrimaryDisplayText = true,
                                                                          .hasSecondaryDisplayText = false}));

    CHECK(shouldShowCompositeMetadataRow(CompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                                          .showEmptyMetadata = false,
                                                                          .primaryEditorEditing = false,
                                                                          .secondaryEditorEditing = false,
                                                                          .hasPrimaryDisplayText = false,
                                                                          .hasSecondaryDisplayText = true}));

    CHECK(shouldShowCompositeMetadataRow(CompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                                          .showEmptyMetadata = true,
                                                                          .primaryEditorEditing = false,
                                                                          .secondaryEditorEditing = false,
                                                                          .hasPrimaryDisplayText = false,
                                                                          .hasSecondaryDisplayText = false}));

    CHECK(shouldShowCompositeMetadataRow(CompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                                          .showEmptyMetadata = false,
                                                                          .primaryEditorEditing = true,
                                                                          .secondaryEditorEditing = false,
                                                                          .hasPrimaryDisplayText = false,
                                                                          .hasSecondaryDisplayText = false}));

    CHECK(shouldShowCompositeMetadataRow(CompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                                          .showEmptyMetadata = false,
                                                                          .primaryEditorEditing = false,
                                                                          .secondaryEditorEditing = true,
                                                                          .hasPrimaryDisplayText = false,
                                                                          .hasSecondaryDisplayText = false}));
  }
} // namespace ao::gtk::layout::track_field_grid::test
