// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/source/SmartListEvaluator.h"
#include "runtime/source/SmartListSource.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ao::rt
{
  class TextOrderingPolicy;
}

namespace ao::rt::test
{
  std::string_view trackGroupHeadingText(TrackGroupHeadingValue const& value);

  std::optional<std::uint16_t> trackGroupHeadingYear(TrackGroupHeadingValue const& value);

  std::optional<MissingTrackValueKind> trackGroupHeadingMissingKind(TrackGroupHeadingValue const& value);

  struct TrackListProjectionFixture final
  {
    MusicLibraryFixture libraryFixture;
    std::shared_ptr<MutableTrackSource> sourcePtr;
    MutableTrackSource& source;
    SmartListEvaluator engine;
    std::shared_ptr<SmartListSource> filteredPtr;

    TrackListProjectionFixture();

    TrackListProjection createProjection(ViewId viewId, TextOrderingPolicy const* textOrderingPolicy = nullptr);

    TrackListProjection createUnfilteredProjection(ViewId viewId,
                                                   TextOrderingPolicy const* textOrderingPolicy = nullptr);

    void setupFiltered(std::span<TrackId const> ids);
  };
} // namespace ao::rt::test
