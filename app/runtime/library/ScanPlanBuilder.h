// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/library/ScanPlan.h>

#include <filesystem>
#include <functional>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class ScanPlanBuilder final
  {
  public:
    using ProgressCallback = std::move_only_function<void(std::filesystem::path const& currentPath)>;

    explicit ScanPlanBuilder(library::MusicLibrary& ml);

    /**
     * Scans the music root recursively and compares with the manifest.
     *
     * Per-file problems (an unreadable entry, a directory we cannot enter) are
     * carried in-band as `ScanClassification::Error` items so the rest of the
     * plan still applies. The `Result` error channel is reserved for failures
     * that prevent any plan from being built at all: a music root that does not
     * exist (`NotFound`) or a filesystem walk that cannot even start
     * (`IoError`). An empty plan is a valid success (an empty library).
     */
    Result<ScanPlan> buildPlan(ProgressCallback progress = nullptr);

  private:
    library::MusicLibrary& _ml;
  };
} // namespace ao::rt
