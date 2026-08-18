// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
  class ReadTransaction;
} // namespace ao::library

namespace ao::rt
{
  /**
   * ResourceCarrierIndex - which files reference each resource, as of one revision.
   *
   * A cover read that misses the derived cache needs some track that references
   * the resource, and nothing indexes that direction. One pass over track cover
   * references builds this, so a miss costs a lookup and one file read rather
   * than a scan.
   *
   * The index is keyed by `ResourceId` rather than by digest, and that is forced
   * rather than chosen: a request arrives as a handle, and resolving it to a
   * digest takes a read transaction, so a digest-keyed index could not be
   * consulted until after the work it is supposed to guide had started. Cover
   * references store handles, so building it from them needs no digest either.
   *
   * A snapshot is immutable once built, so its contents are safe to share with
   * any number of workers without copying. A stale snapshot is harmless rather
   * than merely unlikely: it is a set of files to try, every one still verified
   * by digest, and the worst outcome is attempting a file that no longer carries
   * the content and failing that candidate.
   *
   * Candidate order carries no meaning and must not acquire one. It is ascending
   * track order, which is stable only so repeated attempts behave the same way;
   * correctness comes from the hash, not from which candidate answered.
   */
  class ResourceCarrierIndex final
  {
  public:
    using CarrierMap = std::unordered_map<ResourceId, std::vector<std::string>>;

    ResourceCarrierIndex() = default;
    ResourceCarrierIndex(std::uint64_t libraryRevision, CarrierMap carriers);

    /// The revision this snapshot was built from. A snapshot whose stamp is
    /// behind the library is stale, and the next miss rebuilds.
    std::uint64_t libraryRevision() const noexcept { return _libraryRevision; }

    /// Whether this snapshot answers a request that pinned @p requestRevision.
    ///
    /// Usable is `not older`, never `equal`. A request reads its revision from a
    /// read transaction, which pins one, and then loads the snapshot slot, which
    /// does not, so a snapshot published for a later revision can arrive between
    /// the two loads. This accepts it rather than rebuilding what the library has
    /// already moved past.
    ///
    /// The newer snapshot is not a superset of the older one: a scan, an import,
    /// or a deletion removes references as well as adding them, so accepting it
    /// can cost a candidate the pinned revision still knew. That loss is bounded
    /// and one-directional, because a candidate list is evidence and never
    /// authority — a candidate answers only when its bytes hash to the requested
    /// digest. So a dropped candidate ends in a miss, which the walk reports as
    /// no image, and never in another cover's bytes; and where the reference was
    /// dropped because the library no longer holds it, that miss is the answer
    /// the request should get.
    ///
    /// Both the request that decides whether to rebuild and the rebuild that
    /// re-checks under its lock ask this one question, so they cannot disagree.
    bool answersRevision(std::uint64_t const requestRevision) const noexcept
    {
      return _libraryRevision >= requestRevision;
    }

    /// The URIs of files that reference @p resourceId, empty when none does.
    std::span<std::string const> carrierUris(ResourceId resourceId) const;

    std::size_t resourceCount() const noexcept { return _carriers.size(); }

  private:
    std::uint64_t _libraryRevision = 0;
    CarrierMap _carriers{};
  };

  /**
   * @brief Builds the index from the tracks visible in @p transaction.
   *
   * Reads the revision from the same transaction, so the stamp describes exactly
   * the rows that produced the snapshot. One cold-side pass over track records,
   * on a worker.
   */
  ResourceCarrierIndex buildResourceCarrierIndex(library::MusicLibrary const& library,
                                                 library::ReadTransaction const& transaction);
} // namespace ao::rt
