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
  struct ViewServiceTestEnv final
  {
    TestMusicLibrary library;
    MockExecutor executor;
    async::Runtime runtime;
    LibraryChanges changes;
    LibraryWriter writer;
    std::unique_ptr<TrackSourceCache> cachePtr;

    ViewServiceTestEnv()
      : runtime{executor}
      , changes{}
      , writer{library.library(), changes}
      , cachePtr{std::make_unique<TrackSourceCache>(library.library(), changes)}
    {
    }

    ViewService makeService() { return ViewService{executor, library.library(), *cachePtr}; }
  };
} // namespace ao::rt::test
