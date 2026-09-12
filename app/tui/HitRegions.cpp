// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "HitRegions.h"

#include "MouseBindings.h"
#include <ao/CoreIds.h>

#include <ftxui/screen/box.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace ao::tui
{
  bool hasCoverIntersection(ftxui::Box const& cover, ftxui::Box const& foreground)
  {
    // Kitty clears a one-cell halo around its image when placement changes.
    return !cover.IsEmpty() && !foreground.IsEmpty() && cover.x_min - 1 <= foreground.x_max &&
           cover.x_max + 1 >= foreground.x_min && cover.y_min - 1 <= foreground.y_max &&
           cover.y_max + 1 >= foreground.y_min;
  }

  bool hasHitArea(ftxui::Box const& box)
  {
    return box.x_min <= box.x_max && box.y_min <= box.y_max &&
           (box.x_min != 0 || box.x_max != 0 || box.y_min != 0 || box.y_max != 0);
  }

  bool contains(ftxui::Box const& box, std::int32_t const column, std::int32_t const row)
  {
    return hasHitArea(box) && column >= box.x_min && column <= box.x_max && row >= box.y_min && row <= box.y_max;
  }

  void HitRegions::clearFrameLocalRows()
  {
    navigation = {};
    libraryButtonBox = kEmptyMouseBox;
    presentationButtonBox = kEmptyMouseBox;
    libraryRows.clear();
    navigationPinBox = kEmptyMouseBox;
    navigationDividerBox = kEmptyMouseBox;
    navigationSearchBox = kEmptyMouseBox;
    qualityHoverBox = kEmptyMouseBox;
    detailPanel = {};
    detailSectionBoxes.fill(kEmptyMouseBox);
    detailTrack = kInvalidTrackId;
    detailToggleBox = kEmptyMouseBox;
    detailDividerBox = kEmptyMouseBox;
    overlayPanel = {};
    inputPanel = {};
    completion = {};
    statusActions.clear();
    cancelSelectionBox = kEmptyMouseBox;
    playbackModeBox = kEmptyMouseBox;
    playbackMetadata = {};
    goToMenu = {};
    goToStatus = {};
    trackTableRevision = 0;
    trackRows.clear();
    outputDeviceRows.clear();
    presentationRows.clear();
    notificationDetailRows.clear();
    trackColumnResizeHandles.clear();
    trackSectionRows.clear();
  }

  ButtonHitTestResult HitRegions::hitTestButton(std::int32_t const column,
                                                std::int32_t const row,
                                                HitTestContext const context) const
  {
    if (context.isTextInputActive)
    {
      return {};
    }

    auto result = ButtonHitTestResult{};

    if (!context.isOverlayActive)
    {
      if (contains(playbackModeBox, column, row))
      {
        result.hoveredButton = HoveredButton::PlaybackMode;
        return result;
      }

      auto const metadataButtons = std::array{
        std::pair{&playbackMetadata.title, HoveredButton::PlaybackTitle},
        std::pair{&playbackMetadata.artist, HoveredButton::PlaybackArtist},
        std::pair{&playbackMetadata.album, HoveredButton::PlaybackAlbum},
      };

      for (auto const& [box, button] : metadataButtons)
      {
        if (contains(*box, column, row))
        {
          result.hoveredButton = button;
          return result;
        }
      }

      if (contains(navigationPinBox, column, row) || contains(navigationDividerBox, column, row))
      {
        result.hoveredButton = HoveredButton::NavigationToggle;
        return result;
      }

      if (contains(detailToggleBox, column, row) || contains(detailDividerBox, column, row))
      {
        result.hoveredButton = HoveredButton::DetailToggle;
        return result;
      }
    }

    if (contains(outputDeviceButtonBox, column, row))
    {
      result.hoveredButton = HoveredButton::OutputDevice;
      return result;
    }

    if (contains(soulButtonBox, column, row))
    {
      result.hoveredButton = HoveredButton::Soul;
      result.isQualityHoverVisible = !context.isOverlayActive;
      return result;
    }

    if (contains(libraryButtonBox, column, row))
    {
      result.hoveredButton = HoveredButton::Library;
      return result;
    }

    if (contains(presentationButtonBox, column, row))
    {
      result.hoveredButton = HoveredButton::Presentation;
      return result;
    }

    if (contains(settingsButtonBox, column, row))
    {
      result.hoveredButton = HoveredButton::Settings;
      return result;
    }

    if (contains(activityStatusBox, column, row))
    {
      result.hoveredButton = HoveredButton::ActivityStatus;
      return result;
    }

    return result;
  }
} // namespace ao::tui
