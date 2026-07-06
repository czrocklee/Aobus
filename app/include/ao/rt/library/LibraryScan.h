// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/library/ScanPlan.h>

#include <filesystem>
#include <functional>
#include <stop_token>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  /**
   * Synchronous frontend-shared scan entry point.
   *
   * Plan building and applying are runtime-private implementation details; the
   * public contract is the ScanPlan DTO vocabulary plus this facade.
   */
  class LibraryScan final
  {
  public:
    using BuildProgressCallback = std::move_only_function<void(std::filesystem::path const& path)>;
    using ApplyProgressCallback = std::move_only_function<void(ScanApplyProgress const& progress)>;
    using FailureCallback = std::move_only_function<void(ScanFailure const& failure)>;

    explicit LibraryScan(library::MusicLibrary& library);

    Result<ScanPlan> buildPlan(BuildProgressCallback progress = {});

    Result<ScanApplyResult> applyPlan(ScanPlan plan,
                                      ScanApplyOptions options = {},
                                      ApplyProgressCallback progress = {},
                                      FailureCallback failure = {},
                                      std::stop_token stopToken = {});

  private:
    library::MusicLibrary& _library;
  };
} // namespace ao::rt
