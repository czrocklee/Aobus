// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestSupport.h"
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

namespace ao::uimodel::test
{
  struct TrackPresentationFixture final
  {
    rt::test::MusicLibraryFixture libraryFixture{};
    rt::test::InlineExecutor executor{};
    rt::LibraryChanges changes{};
    rt::TrackSourceCache trackSourceCache{libraryFixture.library(), changes};
    rt::ViewService viewService{executor, libraryFixture.library(), trackSourceCache};
    rt::WorkspaceService workspace{executor, viewService, changes};
    TrackPresentationCatalog catalog{workspace};
    ListPresentationPreferenceStore preferences{catalog};
  };
} // namespace ao::uimodel::test
