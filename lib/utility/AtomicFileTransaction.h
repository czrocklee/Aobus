// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>
#include <utility>

namespace ao::utility::detail
{
  enum class AtomicReplacementMode : std::uint8_t
  {
    Durable,
    VisibilityOnly,
  };

  /**
   * Runs the platform-independent atomic replacement state machine.
   *
   * Operations and its temporary-file result are private implementation seams.
   * The temporary-file destructor owns best-effort cleanup until replaceTarget
   * succeeds, which keeps every pre-replacement return on one RAII path.
   */
  template<typename Operations>
  Result<> runAtomicReplacement(Operations& operations,
                                std::filesystem::path const& targetPath,
                                std::string_view data,
                                AtomicReplacementMode const mode)
  {
    auto normalizedTargetRes = operations.normalizeTargetPath(targetPath);

    if (!normalizedTargetRes)
    {
      return std::unexpected{normalizedTargetRes.error()};
    }

    auto const& normalizedTarget = *normalizedTargetRes;
    auto const parentPath = normalizedTarget.parent_path();

    if (auto const result = operations.createParentDirectories(parentPath); !result)
    {
      return result;
    }

    auto temporaryFileRes = operations.createPrivateTemporaryFile(parentPath);

    if (!temporaryFileRes)
    {
      return std::unexpected{temporaryFileRes.error()};
    }

    auto temporaryFile = std::move(*temporaryFileRes);

    if (auto const result = temporaryFile.writeAll(data); !result)
    {
      return result;
    }

    if (mode == AtomicReplacementMode::Durable)
    {
      if (auto const result = temporaryFile.synchronizeData(); !result)
      {
        return result;
      }
    }

    if (auto const result = temporaryFile.closeForReplacement(); !result)
    {
      return result;
    }

    if (auto const result = temporaryFile.replaceTarget(normalizedTarget, mode); !result)
    {
      return result;
    }

    if (mode == AtomicReplacementMode::Durable)
    {
      static_assert(noexcept(operations.synchronizeParentDirectoryBestEffort(parentPath)));
      operations.synchronizeParentDirectoryBestEffort(parentPath);
    }

    return {};
  }
} // namespace ao::utility::detail
