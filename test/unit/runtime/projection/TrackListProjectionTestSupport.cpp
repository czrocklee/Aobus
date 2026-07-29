// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/projection/TrackListProjectionTestSupport.h"

#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace ao::rt::test
{
  std::string_view trackGroupHeadingText(TrackGroupHeadingValue const& value)
  {
    auto const* text = std::get_if<std::string>(&value);
    return text != nullptr ? std::string_view{*text} : std::string_view{};
  }

  std::optional<std::uint16_t> trackGroupHeadingYear(TrackGroupHeadingValue const& value)
  {
    if (auto const* year = std::get_if<std::uint16_t>(&value); year != nullptr)
    {
      return *year;
    }

    return std::nullopt;
  }

  std::optional<MissingTrackValueKind> trackGroupHeadingMissingKind(TrackGroupHeadingValue const& value)
  {
    if (auto const* kind = std::get_if<MissingTrackValueKind>(&value); kind != nullptr)
    {
      return *kind;
    }

    return std::nullopt;
  }

  TrackListProjectionFixture::TrackListProjectionFixture()
    : sourcePtr{std::make_shared<MutableTrackSource>()}, source{*sourcePtr}, engine{libraryFixture.library()}
  {
  }

  TrackListProjection TrackListProjectionFixture::createProjection(ViewId const viewId)
  {
    return TrackListProjection{viewId, TrackSourceLease{filteredPtr}, libraryFixture.library()};
  }

  TrackListProjection TrackListProjectionFixture::createUnfilteredProjection(ViewId const viewId)
  {
    return TrackListProjection{viewId, TrackSourceLease{sourcePtr}, libraryFixture.library()};
  }

  void TrackListProjectionFixture::setupFiltered(std::span<TrackId const> const ids)
  {
    source.setInitial(ids);
    filteredPtr = std::make_shared<SmartListSource>(TrackSourceLease{sourcePtr}, engine);
    filteredPtr->reload();
  }
} // namespace ao::rt::test
