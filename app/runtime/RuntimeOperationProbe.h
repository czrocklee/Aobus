// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>

namespace ao::rt
{
  class IndexedTrackSequence;
  class SmartListEvaluator;
  class TrackListProjection;
  class TrackSourceCache;

  namespace detail
  {
    struct IndexedTrackSequenceOperationCounts final
    {
      std::size_t indexRebuilds = 0;
      std::size_t incrementalScriptApplications = 0;
    };

    struct SmartListEvaluatorOperationCounts final
    {
      std::size_t upstreamIndexRebuilds = 0;
      std::size_t membershipIndexRebuilds = 0;
    };

    struct TrackListProjectionOperationCounts final
    {
      std::uint64_t fullProjectionRebuilds = 0;
      std::uint64_t incrementalProjectionUpdates = 0;
      std::uint64_t arenaRebases = 0;
      std::uint64_t rowIndexRebuilds = 0;

      bool operator==(TrackListProjectionOperationCounts const&) const = default;
    };

    struct TrackSourceCacheOperationCounts final
    {
      std::size_t expiredAdHocSourcesPruned = 0;
    };

    /** Source-private deterministic instrumentation for incremental-behavior tests. */
    class RuntimeOperationProbe final
    {
    public:
      static IndexedTrackSequenceOperationCounts counts(IndexedTrackSequence const& sequence) noexcept;
      static SmartListEvaluatorOperationCounts counts(SmartListEvaluator const& evaluator) noexcept;
      static TrackListProjectionOperationCounts counts(TrackListProjection const& projection) noexcept;
      static TrackSourceCacheOperationCounts counts(TrackSourceCache const& cache) noexcept;
    };
  } // namespace detail
} // namespace ao::rt
