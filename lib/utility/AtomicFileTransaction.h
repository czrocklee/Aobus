// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <expected>
#include <filesystem>
#include <string_view>
#include <utility>

namespace ao::utility::detail
{
  /**
   * Runs the platform-independent atomic replacement state machine.
   *
   * Operations and its temporary-file result are private implementation seams.
   * The temporary-file destructor owns best-effort cleanup until replaceTarget
   * succeeds, which keeps every pre-replacement return on one RAII path.
   */
  template<typename Operations>
  Result<> runAtomicReplacement(Operations& operations, std::filesystem::path const& targetPath, std::string_view data)
  {
    auto normalizedTargetResult = operations.normalizeTargetPath(targetPath);

    if (!normalizedTargetResult)
    {
      return std::unexpected{normalizedTargetResult.error()};
    }

    auto const& normalizedTarget = *normalizedTargetResult;
    auto const parentPath = normalizedTarget.parent_path();

    if (auto const result = operations.createParentDirectories(parentPath); !result)
    {
      return result;
    }

    auto temporaryFileResult = operations.createPrivateTemporaryFile(parentPath);

    if (!temporaryFileResult)
    {
      return std::unexpected{temporaryFileResult.error()};
    }

    auto temporaryFile = std::move(*temporaryFileResult);

    if (auto const result = temporaryFile.writeAll(data); !result)
    {
      return result;
    }

    if (auto const result = temporaryFile.synchronizeData(); !result)
    {
      return result;
    }

    if (auto const result = temporaryFile.closeForReplacement(); !result)
    {
      return result;
    }

    if (auto const result = temporaryFile.replaceTarget(normalizedTarget); !result)
    {
      return result;
    }

    static_assert(noexcept(operations.synchronizeParentDirectoryBestEffort(parentPath)));
    operations.synchronizeParentDirectoryBestEffort(parentPath);
    return {};
  }
} // namespace ao::utility::detail
