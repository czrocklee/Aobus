// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <filesystem>

namespace ao::desktop
{
  struct LibrarySwitchRequest final
  {
    std::filesystem::path libraryRoot;
    bool scanAfterOpen = false;

    friend bool operator==(LibrarySwitchRequest const&, LibrarySwitchRequest const&) = default;
  };

  enum class LibrarySwitchDisposition : std::uint8_t
  {
    ReuseActive,
    Restart,
  };

  struct LibrarySwitchPlan final
  {
    LibrarySwitchDisposition disposition = LibrarySwitchDisposition::ReuseActive;
    LibrarySwitchRequest request;

    friend bool operator==(LibrarySwitchPlan const&, LibrarySwitchPlan const&) = default;
  };

  /** Validate and normalize a desktop Open Library request. */
  Result<LibrarySwitchPlan> planLibrarySwitch(std::filesystem::path const& activeRoot,
                                              std::filesystem::path requestedRoot,
                                              bool scanAfterOpen);
} // namespace ao::desktop
