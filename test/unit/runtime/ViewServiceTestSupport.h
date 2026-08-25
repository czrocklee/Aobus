// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/LoopExecutor.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <memory>

namespace ao::rt::test
{
  struct ViewServiceFixture final
  {
    MusicLibraryFixture libraryFixture;
    async::LoopExecutor executor;
    LibraryChanges changes;
    LibraryWriterFixture writerFixture;
    std::unique_ptr<TrackSourceCache> cachePtr;
    ViewService service;
    WorkspaceService workspace;

    ViewServiceFixture();

    LibraryWriter& writer();

    TrackId addTrack(library::test::TrackSpec const& spec);

    void drainCallbacks();

    ViewId requireView(TrackListViewConfig const& config = {});

    std::shared_ptr<TrackListProjection> requireProjection(ViewId viewId);
  };
} // namespace ao::rt::test
