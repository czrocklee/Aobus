// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/runtime/ViewServiceTestSupport.h"

#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <memory>

namespace ao::rt::test
{
  ViewServiceFixture::ViewServiceFixture()
    : changes{executor, 0, "test-library"}
    , commandsFixture{libraryFixture.library(), changes, executor}
    , cachePtr{std::make_unique<TrackSourceCache>(libraryFixture.library(), changes)}
    , service{executor, libraryFixture.library(), *cachePtr, changes}
    , workspace{executor, service, libraryFixture.library(), changes}
  {
  }

  ViewServiceFixture::~ViewServiceFixture() = default;

  LibraryCommands& ViewServiceFixture::commands()
  {
    return commandsFixture.commands();
  }

  TrackId ViewServiceFixture::addTrack(library::test::TrackSpec const& spec)
  {
    return commandsFixture.addTrack(spec);
  }

  void ViewServiceFixture::drainCallbacks()
  {
    while (executor.tryRunReadyTurn())
    {
    }
  }

  ViewId ViewServiceFixture::requireView(TrackListViewConfig const& config)
  {
    auto request = NavigationRequest{
      .target = FilteredListTarget{.listId = config.listId, .filterExpression = config.filterExpression},
    };

    if (config.optPresentation)
    {
      request.optPresentation = NavigationPresentation{.spec = *config.optPresentation};
    }

    auto const viewId = ao::test::requireValue(workspace.navigate(request));
    drainCallbacks();
    return viewId;
  }

  std::shared_ptr<TrackListProjection const> ViewServiceFixture::requireProjection(ViewId const viewId) const
  {
    return ao::test::requireValue(service.findTrackListProjection(viewId));
  }
} // namespace ao::rt::test
