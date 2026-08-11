// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/LibrarySwitch.h>

#include <ao/Error.h>
#include <ao/desktop/LibraryPath.h>

#include <expected>
#include <filesystem>
#include <utility>

namespace ao::desktop
{
  Result<LibrarySwitchPlan> planLibrarySwitch(std::filesystem::path const& activeRoot,
                                              std::filesystem::path requestedRoot,
                                              bool const scanAfterOpen)
  {
    auto activeRes = normalizeLibraryRoot(activeRoot);

    if (!activeRes)
    {
      return std::unexpected{activeRes.error()};
    }

    auto requestedRes = normalizeExistingLibraryRoot(std::move(requestedRoot));

    if (!requestedRes)
    {
      return std::unexpected{requestedRes.error()};
    }

    return LibrarySwitchPlan{
      .disposition = sameLibraryRoot(*activeRes, *requestedRes) ? LibrarySwitchDisposition::ReuseActive
                                                                : LibrarySwitchDisposition::Restart,
      .request = LibrarySwitchRequest{.libraryRoot = std::move(*requestedRes), .scanAfterOpen = scanAfterOpen},
    };
  }
} // namespace ao::desktop
