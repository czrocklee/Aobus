// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace ao::rt::test
{
  inline std::string_view trackGroupHeadingText(TrackGroupHeadingValue const& value)
  {
    auto const* text = std::get_if<std::string>(&value);
    return text != nullptr ? std::string_view{*text} : std::string_view{};
  }

  inline std::optional<std::uint16_t> trackGroupHeadingYear(TrackGroupHeadingValue const& value)
  {
    if (auto const* year = std::get_if<std::uint16_t>(&value); year != nullptr)
    {
      return *year;
    }

    return std::nullopt;
  }

  inline std::optional<MissingTrackValueKind> trackGroupHeadingMissingKind(TrackGroupHeadingValue const& value)
  {
    if (auto const* kind = std::get_if<MissingTrackValueKind>(&value); kind != nullptr)
    {
      return *kind;
    }

    return std::nullopt;
  }

  struct TrackListProjectionFixture final
  {
    MusicLibraryFixture libraryFixture;
    std::shared_ptr<MutableTrackSource> sourcePtr;
    MutableTrackSource& source;
    SmartListEvaluator engine;
    std::shared_ptr<SmartListSource> filteredPtr;

    TrackListProjectionFixture()
      : sourcePtr{std::make_shared<MutableTrackSource>()}, source{*sourcePtr}, engine{libraryFixture.library()}
    {
    }

    LiveTrackListProjection createProjection(ViewId viewId)
    {
      return LiveTrackListProjection{viewId, TrackSourceLease{filteredPtr}, libraryFixture.library()};
    }

    LiveTrackListProjection createUnfilteredProjection(ViewId viewId)
    {
      return LiveTrackListProjection{viewId, TrackSourceLease{sourcePtr}, libraryFixture.library()};
    }

    void setupFiltered(std::span<TrackId const> ids)
    {
      source.setInitial(ids);
      filteredPtr = std::make_shared<SmartListSource>(TrackSourceLease{sourcePtr}, engine);
      filteredPtr->reload();
    }
  };
} // namespace ao::rt::test
