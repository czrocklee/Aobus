// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/LibraryStartupPlanner.h>

#include <ao/Error.h>
#include <ao/desktop/LibraryPath.h>

#include <expected>
#include <utility>

namespace ao::desktop
{
  Result<LibraryStartupPlan> planLibraryStartup(LibraryStartupInputs inputs)
  {
    if (inputs.optSuccessorRequest)
    {
      auto rootRes = normalizeExistingLibraryRoot(inputs.optSuccessorRequest->libraryRoot);

      if (!rootRes)
      {
        return std::unexpected{rootRes.error()};
      }

      return LibraryStartupPlan{
        .libraryRoot = *rootRes,
        .source = LibraryStartupRootSource::ExplicitSuccessor,
        .playbackPersistence = PlaybackPersistenceStartup::AwaitDurableRoot,
        .optSelectedRootCommit = *rootRes,
        .scanAfterOpen = inputs.optSuccessorRequest->scanAfterOpen,
      };
    }

    if (inputs.optPersistedRoot)
    {
      if (auto rootRes = normalizeExistingLibraryRoot(*inputs.optPersistedRoot); rootRes)
      {
        return LibraryStartupPlan{
          .libraryRoot = std::move(*rootRes),
          .source = LibraryStartupRootSource::Persisted,
          .playbackPersistence = PlaybackPersistenceStartup::Restore,
        };
      }
    }

    auto fallbackRes = normalizeLibraryRoot(std::move(inputs.emptyLibraryRoot));

    if (!fallbackRes)
    {
      return std::unexpected{fallbackRes.error()};
    }

    return LibraryStartupPlan{
      .libraryRoot = std::move(*fallbackRes),
      .source = LibraryStartupRootSource::EmptyLibraryFallback,
      .playbackPersistence = PlaybackPersistenceStartup::Restore,
    };
  }
} // namespace ao::desktop
