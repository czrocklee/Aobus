// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace ao::desktop
{
  enum class LibraryStartupRootSource : std::uint8_t
  {
    ExplicitSuccessor,
    Persisted,
    EmptyLibraryFallback,
  };

  enum class PlaybackPersistenceStartup : std::uint8_t
  {
    Restore,
    AwaitDurableRoot,
  };

  struct LibraryStartupInputs final
  {
    std::optional<LibrarySwitchRequest> optSuccessorRequest{};
    std::optional<std::filesystem::path> optPersistedRoot{};
    std::filesystem::path emptyLibraryRoot;
  };

  struct LibraryStartupPlan final
  {
    std::filesystem::path libraryRoot;
    LibraryStartupRootSource source = LibraryStartupRootSource::EmptyLibraryFallback;
    PlaybackPersistenceStartup playbackPersistence = PlaybackPersistenceStartup::Restore;
    std::optional<std::filesystem::path> optSelectedRootCommit{};
    bool scanAfterOpen = false;

    friend bool operator==(LibraryStartupPlan const&, LibraryStartupPlan const&) = default;
  };

  /** Select one library root without creating directories or mutating persisted settings. */
  Result<LibraryStartupPlan> planLibraryStartup(LibraryStartupInputs inputs);
} // namespace ao::desktop
