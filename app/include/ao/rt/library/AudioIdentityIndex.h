// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace ao::rt
{
  struct AudioIdentityIndexProgress final
  {
    /// File currently being fingerprinted.
    std::filesystem::path path{};
    /// Rows finished so far (completed, skipped, or failed). With concurrent
    /// fingerprinting this is the only monotonic notion of progress; there is
    /// no stable per-item ordering.
    std::int32_t processedCount = 0;
    /// Pending rows counted when indexing started. Best-effort snapshot: rows
    /// added while indexing runs are not included.
    std::int32_t totalCount = 0;
    /// Hash progress within `path`, in [0.0, 1.0].
    double itemFraction = 0.0;
  };

  struct AudioIdentityIndexResult final
  {
    std::int32_t completedCount = 0;
    std::int32_t skippedCount = 0;
    std::int32_t failureCount = 0;
    bool cancelled = false;
  };

  struct AudioIdentityIndexFailure final
  {
    std::string uri{};
    std::string stage{};
    std::string message{};
  };

  using AudioIdentityIndexProgressCallback = std::move_only_function<void(AudioIdentityIndexProgress const& progress)>;
  using AudioIdentityIndexFailureCallback = std::move_only_function<void(AudioIdentityIndexFailure const& failure)>;
} // namespace ao::rt
