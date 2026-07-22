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
    InlineExecutor executor;
    LibraryChanges changes;
    LibraryWriterFixture writerFixture;
    std::unique_ptr<TrackSourceCache> cachePtr;

    ViewServiceFixture()
      : changes{}
      , writerFixture{libraryFixture.library(), changes}
      , cachePtr{std::make_unique<TrackSourceCache>(libraryFixture.library(), changes)}
    {
    }

    LibraryWriter& writer() { return writerFixture.writer(); }
    ViewService makeService() { return ViewService{executor, libraryFixture.library(), *cachePtr}; }

    ViewId requireView(ViewService& service, TrackListViewConfig const& config = {})
    {
      return ao::test::requireValue(service.createView(config));
    }
  };
} // namespace ao::rt::test
