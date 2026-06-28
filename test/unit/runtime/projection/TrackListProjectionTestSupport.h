// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>

#include <memory>
#include <span>

namespace ao::rt::test
{
  struct TestEnv final
  {
    TestMusicLibrary lib;
    MutableTrackSource source;
    SmartListEvaluator engine;
    std::unique_ptr<SmartListSource> filteredPtr;

    TestEnv()
      : engine{lib.library()}
    {
    }

    TrackListProjection createProjection(ViewId viewId)
    {
      return TrackListProjection{viewId, *filteredPtr, lib.library()};
    }

    void setupFiltered(std::span<TrackId const> ids)
    {
      for (auto id : ids)
      {
        source.addInitial(id);
      }

      filteredPtr = std::make_unique<SmartListSource>(source, lib.library(), engine);
      filteredPtr->reload();
    }
  };
} // namespace ao::rt::test
