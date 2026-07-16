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
#include <span>

namespace ao::rt::test
{
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
