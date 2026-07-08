// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestSupport.h"
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

namespace ao::uimodel::test
{
  struct TrackPresentationFixture final
  {
    rt::test::TestMusicLibrary testLib{};
    rt::test::MockExecutor executor{};
    rt::LibraryChanges changes{};
    rt::ListSourceStore listSourceStore{testLib.library(), changes};
    rt::ViewService viewService{executor, testLib.library(), listSourceStore};
    rt::NotificationService notifications;
    rt::PlaybackService playbackService{executor, viewService, testLib.library(), notifications};
    rt::WorkspaceService workspace{viewService, playbackService, changes, testLib.library()};
    TrackPresentationCatalog catalog{workspace};
    ListPresentationPreferenceStore preferences{catalog};
  };
} // namespace ao::uimodel::test
