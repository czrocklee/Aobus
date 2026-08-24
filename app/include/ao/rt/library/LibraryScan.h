// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/library/ScanPlan.h>

#include <filesystem>
#include <stop_token>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  /**
   * Synchronous scan-plan builder.
   *
   * Applying a plan is a runtime-private mutation operation; the public
   * contract is the opaque ScanPlan capability plus this read-only facade.
   */
  class LibraryScan final
  {
  public:
    using BuildProgressCallback = compat::MoveOnlyFunction<void(std::filesystem::path const& path)>;

    explicit LibraryScan(library::MusicLibrary const& library);

    Result<ScanPlan> buildPlan(BuildProgressCallback progress = {}, std::stop_token stopToken = {});

  private:
    Result<ScanPlan> buildPlanUnchecked(BuildProgressCallback progress, std::stop_token stopToken);

    library::MusicLibrary const& _library;
  };
} // namespace ao::rt
