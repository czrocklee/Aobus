// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
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
    ViewService service;
    WorkspaceService workspace;

    ViewServiceFixture()
      : changes{executor, 0}
      , writerFixture{libraryFixture.library(), changes}
      , cachePtr{std::make_unique<TrackSourceCache>(libraryFixture.library(), changes)}
      , service{executor, libraryFixture.library(), *cachePtr}
      , workspace{executor, service, changes}
    {
    }

    LibraryWriter& writer() { return writerFixture.writer(); }

    TrackId addTrack(library::test::TrackSpec const& spec)
    {
      return addTrackAndPublish(libraryFixture.library(), changes, spec);
    }

    ViewId requireView(TrackListViewConfig const& config = {})
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

    std::shared_ptr<TrackListProjection> requireProjection(ViewId const viewId)
    {
      return ao::test::requireValue(service.findTrackListProjection(viewId));
    }
  };
} // namespace ao::rt::test
