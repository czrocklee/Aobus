// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/ResourceLayout.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace ao::rt
{
  class ResourceDiskCache;

  /// What one materialization needs that it does not compute itself. Passive and
  /// non-owning: every reference outlives the call.
  struct ResourceMaterializationContext final
  {
    /// The row the handle resolved to. Its digest is the whole acceptance test.
    library::ResourceDescriptor descriptor;

    /// Files that reference this resource, in the index's stable order.
    std::span<std::string const> candidateUris;

    /// Where a relative manifest URI resolves, so a relocated root works.
    std::filesystem::path const& musicRoot;

    ResourceDiskCache const& cache;

    /// The ceiling this request is subject to. Absent for administrative CLI
    /// export, which the delivery specification exempts from the interactive
    /// limits; the walk itself has no ceiling of its own.
    std::optional<std::size_t> optMaximumBytes;
  };

  /**
   * @brief Produces the bytes a resource names, or reports that none can be.
   *
   * Walks one content-addressed store by cost: the derived cache first, then
   * every referencing carrier. A hit at either tier is accepted on the same
   * terms — its bytes hash to the descriptor's digest — so no source is trusted
   * and no source is preferred.
   *
   * A failed source is never a failed request. Only bytes that verify, or
   * candidates running out, end the walk; a missing file, an unparsable one, and
   * a carrier whose art has changed all advance to the next candidate, because a
   * source is evidence and not an authority.
   *
   * Returns no bytes when nothing can reproduce the content, `ValueTooLarge` when
   * what was produced exceeds the caller's ceiling, and throws
   * `async::OperationCancelled` when @p stopToken is signalled. Cancellation is
   * checked between candidates rather than once at entry, because a walk over
   * dozens of carriers on slow media is exactly where a shutdown must stop.
   *
   * Opens no read transaction: the caller resolves the descriptor and the
   * candidate snapshot first and closes its transaction, because a long-lived
   * read snapshot holds back page reuse for every concurrent writer.
   */
  Result<std::optional<std::vector<std::byte>>> materializeResource(ResourceMaterializationContext const& context,
                                                                    std::stop_token const& stopToken);
} // namespace ao::rt
