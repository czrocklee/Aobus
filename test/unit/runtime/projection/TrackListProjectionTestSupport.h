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

#include <memory>
#include <span>

namespace ao::rt::test
{
  struct TrackListProjectionFixture final
  {
    MusicLibraryFixture libraryFixture;
    MutableTrackSource source;
    SmartListEvaluator engine;
    std::unique_ptr<SmartListSource> filteredPtr;

    TrackListProjectionFixture()
      : engine{libraryFixture.library()}
    {
    }

    LiveTrackListProjection createProjection(ViewId viewId)
    {
      return LiveTrackListProjection{viewId, *filteredPtr, libraryFixture.library()};
    }

    void setupFiltered(std::span<TrackId const> ids)
    {
      for (auto id : ids)
      {
        source.addInitial(id);
      }

      filteredPtr = std::make_unique<SmartListSource>(source, libraryFixture.library(), engine);
      filteredPtr->reload();
    }
  };
} // namespace ao::rt::test
