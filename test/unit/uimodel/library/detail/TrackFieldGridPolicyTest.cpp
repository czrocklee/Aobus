// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/detail/TrackFieldGridPolicy.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("TrackFieldGridPolicy renders sections from available rows and selection state",
            "[uimodel][unit][library][detail]")
  {
    CHECK_FALSE(shouldRenderTrackFieldGridMetadataSection(TrackFieldGridSectionAvailability{
      .hasMetadataFields = false, .hasSelectedTracks = true, .hasTechnicalFields = true}));
    CHECK(shouldRenderTrackFieldGridMetadataSection(TrackFieldGridSectionAvailability{
      .hasMetadataFields = true, .hasSelectedTracks = false, .hasTechnicalFields = false}));

    CHECK_FALSE(shouldRenderTrackFieldGridCustomSection(TrackFieldGridSectionAvailability{
      .hasMetadataFields = true, .hasSelectedTracks = false, .hasTechnicalFields = true}));
    CHECK(shouldRenderTrackFieldGridCustomSection(TrackFieldGridSectionAvailability{
      .hasMetadataFields = false, .hasSelectedTracks = true, .hasTechnicalFields = false}));

    CHECK_FALSE(shouldRenderTrackFieldGridTechnicalSection(TrackFieldGridSectionAvailability{
      .hasMetadataFields = true, .hasSelectedTracks = true, .hasTechnicalFields = false}));
    CHECK(shouldRenderTrackFieldGridTechnicalSection(TrackFieldGridSectionAvailability{
      .hasMetadataFields = false, .hasSelectedTracks = false, .hasTechnicalFields = true}));
  }

  TEST_CASE("TrackFieldGridPolicy shows metadata rows from expansion and content state",
            "[uimodel][unit][library][detail]")
  {
    CHECK_FALSE(shouldShowTrackFieldGridMetadataFieldRow(TrackFieldGridMetadataFieldVisibility{
      .metadataExpanded = false, .showEmptyMetadata = true, .editorEditing = true, .hasDisplayText = true}));

    CHECK_FALSE(shouldShowTrackFieldGridMetadataFieldRow(TrackFieldGridMetadataFieldVisibility{
      .metadataExpanded = true, .showEmptyMetadata = false, .editorEditing = false, .hasDisplayText = false}));

    CHECK(shouldShowTrackFieldGridMetadataFieldRow(TrackFieldGridMetadataFieldVisibility{
      .metadataExpanded = true, .showEmptyMetadata = false, .editorEditing = false, .hasDisplayText = true}));

    CHECK(shouldShowTrackFieldGridMetadataFieldRow(TrackFieldGridMetadataFieldVisibility{
      .metadataExpanded = true, .showEmptyMetadata = true, .editorEditing = false, .hasDisplayText = false}));

    CHECK(shouldShowTrackFieldGridMetadataFieldRow(TrackFieldGridMetadataFieldVisibility{
      .metadataExpanded = true, .showEmptyMetadata = false, .editorEditing = true, .hasDisplayText = false}));
  }

  TEST_CASE("TrackFieldGridPolicy shows composite metadata rows from either side", "[uimodel][unit][library][detail]")
  {
    CHECK_FALSE(shouldShowTrackFieldGridCompositeMetadataRow(
      TrackFieldGridCompositeMetadataFieldVisibility{.metadataExpanded = false,
                                                     .showEmptyMetadata = true,
                                                     .primaryEditorEditing = true,
                                                     .secondaryEditorEditing = true,
                                                     .hasPrimaryDisplayText = true,
                                                     .hasSecondaryDisplayText = true}));

    CHECK_FALSE(shouldShowTrackFieldGridCompositeMetadataRow(
      TrackFieldGridCompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                     .showEmptyMetadata = false,
                                                     .primaryEditorEditing = false,
                                                     .secondaryEditorEditing = false,
                                                     .hasPrimaryDisplayText = false,
                                                     .hasSecondaryDisplayText = false}));

    CHECK(shouldShowTrackFieldGridCompositeMetadataRow(
      TrackFieldGridCompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                     .showEmptyMetadata = false,
                                                     .primaryEditorEditing = false,
                                                     .secondaryEditorEditing = false,
                                                     .hasPrimaryDisplayText = true,
                                                     .hasSecondaryDisplayText = false}));

    CHECK(shouldShowTrackFieldGridCompositeMetadataRow(
      TrackFieldGridCompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                     .showEmptyMetadata = false,
                                                     .primaryEditorEditing = false,
                                                     .secondaryEditorEditing = false,
                                                     .hasPrimaryDisplayText = false,
                                                     .hasSecondaryDisplayText = true}));

    CHECK(shouldShowTrackFieldGridCompositeMetadataRow(
      TrackFieldGridCompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                     .showEmptyMetadata = true,
                                                     .primaryEditorEditing = false,
                                                     .secondaryEditorEditing = false,
                                                     .hasPrimaryDisplayText = false,
                                                     .hasSecondaryDisplayText = false}));

    CHECK(shouldShowTrackFieldGridCompositeMetadataRow(
      TrackFieldGridCompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                     .showEmptyMetadata = false,
                                                     .primaryEditorEditing = true,
                                                     .secondaryEditorEditing = false,
                                                     .hasPrimaryDisplayText = false,
                                                     .hasSecondaryDisplayText = false}));

    CHECK(shouldShowTrackFieldGridCompositeMetadataRow(
      TrackFieldGridCompositeMetadataFieldVisibility{.metadataExpanded = true,
                                                     .showEmptyMetadata = false,
                                                     .primaryEditorEditing = false,
                                                     .secondaryEditorEditing = true,
                                                     .hasPrimaryDisplayText = false,
                                                     .hasSecondaryDisplayText = false}));
  }
} // namespace ao::uimodel::test
