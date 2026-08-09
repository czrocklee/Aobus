// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/ViewServiceTestSupport.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <memory>
#include <utility>

namespace ao::rt::test
{
  ViewServiceFixture::ViewServiceFixture()
    : changes{executor, 0, "test-library"}
    , writerFixture{libraryFixture.library(), changes}
    , cachePtr{std::make_unique<TrackSourceCache>(libraryFixture.library(), changes)}
    , service{executor, libraryFixture.library(), *cachePtr, changes}
    , workspace{executor, service, changes}
  {
  }

  LibraryWriter& ViewServiceFixture::writer()
  {
    return writerFixture.writer();
  }

  TrackId ViewServiceFixture::addTrack(library::test::TrackSpec const& spec)
  {
    return addTrackAndPublish(libraryFixture.library(), changes, spec);
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
    else if (config.groupBy != TrackGroupKey::None || !config.sortBy.empty())
    {
      auto presentation = defaultTrackPresentationSpec();

      if (config.groupBy != TrackGroupKey::None)
      {
        for (auto const& preset : builtinTrackPresentationPresets())
        {
          if (preset.spec.groupBy == config.groupBy)
          {
            presentation = preset.spec;
            break;
          }
        }
      }

      if (!config.sortBy.empty())
      {
        presentation.sortBy = config.sortBy;
      }

      request.optPresentation = NavigationPresentation{.spec = std::move(presentation)};
    }

    return ao::test::requireValue(workspace.navigate(request));
  }

  std::shared_ptr<TrackListProjection> ViewServiceFixture::requireProjection(ViewId const viewId)
  {
    return ao::test::requireValue(service.findTrackListProjection(viewId));
  }
} // namespace ao::rt::test
