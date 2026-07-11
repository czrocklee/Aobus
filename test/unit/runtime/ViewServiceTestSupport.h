// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <memory>

namespace ao::rt::test
{
  struct ViewServiceFixture final
  {
    MusicLibraryFixture libraryFixture;
    MockExecutor executor;
    async::Runtime runtime;
    LibraryChanges changes;
    LibraryWriter writer;
    std::unique_ptr<TrackSourceCache> cachePtr;

    ViewServiceFixture()
      : runtime{executor}
      , changes{}
      , writer{libraryFixture.library(), changes}
      , cachePtr{std::make_unique<TrackSourceCache>(libraryFixture.library(), changes)}
    {
    }

    ViewService makeService() { return ViewService{executor, libraryFixture.library(), *cachePtr}; }

    CreateTrackListViewReply requireView(ViewService& service,
                                         TrackListViewConfig const& config = {},
                                         bool const attached = true)
    {
      return ao::test::requireValue(service.createView(config, attached));
    }
  };
} // namespace ao::rt::test
