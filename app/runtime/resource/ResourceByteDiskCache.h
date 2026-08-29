// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/utility/Sha256.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ao::rt
{
  /**
   * @brief Where cover entries live under an application cache root.
   *
   * A composition root supplies the cache root; which subdirectory covers occupy
   * is a path derived from a supplied root, which the runtime owns. Deciding it
   * here keeps it one decision rather than one per frontend, and lets anything
   * that needs to name the same location agree with the runtime.
   *
   * An empty root stays empty: that is how a frontend says it has no cache.
   */
  std::filesystem::path coverCacheDirectory(std::filesystem::path const& cacheDirectory);

  /**
   * ResourceByteDiskCache - encoded cover bytes kept on disk, named by their digest.
   *
   * The cache is content-addressed, which is what makes it the cheap tier of one
   * store rather than a second mechanism: an entry is served because its bytes
   * hash to the name it was asked for, exactly as a media file's picture is. So
   * no entry can serve one cover's bytes for another cover's reference, and one
   * cache serves every library on the machine — two libraries holding the same
   * album share one entry.
   *
   * Nothing here is authoritative. Deleting the directory loses no library fact;
   * what it can change is what is currently displayed, because an entry may be
   * the last readable copy of bytes whose carrier files are gone.
   *
   * Every failure is silent by contract. A caller reaches `store` holding bytes
   * it has already verified, so a cache that cannot retain them must not fail
   * the request that produced them: a failed write installs no entry and the next
   * request is another cold miss, and a failed enumeration or removal leaves
   * convergence on the budget deferred to a later pass that succeeds. A
   * permanently read-only directory therefore never converges and never fails a
   * cover request.
   *
   * One instance is shared across workers. The only state the type keeps is the
   * approximate number of bytes written since the last convergence pass, which
   * is a scheduling hint rather than a fact about the directory: losing an
   * update to it defers a pass, and never serves or drops an entry.
   */
  class ResourceByteDiskCache final
  {
  public:
    static constexpr std::size_t kDefaultByteBudget = std::size_t{256U} * 1024U * 1024U;

    /// A hit rewrites the entry's modification time at most this often, so a
    /// warm cover is not rewritten on every display.
    static constexpr auto kTouchInterval = std::chrono::hours{24};

    /// Convergence walks the whole directory, so it runs once per this fraction
    /// of the budget written rather than once per write: a library browsed for
    /// the first time stores covers in bursts, and walking after each one costs
    /// more than the bytes the walk exists to reclaim. Deferring adds at most one
    /// such share to what one process holds beyond the budget between two passes
    /// that succeed; it adds nothing to the two overshoots the budget already
    /// admits, several processes writing at once and a directory whose passes
    /// keep failing, both of which are unbounded with or without this. A budget
    /// small enough for the share to round to zero converges after every write,
    /// which is what a test-sized cache wants.
    static constexpr std::size_t kConvergeWriteShare = 16;

    struct Config final
    {
      /// The cache root, which a composition root resolves and the runtime never
      /// discovers. Empty means this build of the walk has one tier: a request
      /// that misses the caller's in-process cache re-extracts from a carrier,
      /// which costs latency, and costs the image itself for content whose
      /// carriers are all gone.
      std::filesystem::path directory{};

      /// Converged toward rather than held exactly, by a pass this process runs
      /// once per `kConvergeWriteShare` of the budget it writes. Several
      /// processes may each observe room and together exceed it; closing that
      /// would need a cross-process protocol worth more than the bytes it saves.
      std::size_t byteBudget = kDefaultByteBudget;

      /// The largest entry the cache will hold. Required: the cache keeps only
      /// what interactive delivery may serve, so one cover no frontend can
      /// display cannot take a share of the budget.
      std::size_t maximumEntryBytes = 0;
    };

    explicit ResourceByteDiskCache(Config config);

    bool isEnabled() const noexcept { return !_config.directory.empty(); }

    /**
     * @brief Bytes that hash to @p digest, or nothing.
     *
     * An entry whose content does not match its name is discarded rather than
     * served, which is the general acceptance rule applied to the cache rather
     * than a rule about caches. A discard that fails to delete still reports a
     * miss, so the walk continues to the carriers either way.
     */
    std::optional<std::vector<std::byte>> read(utility::Sha256Digest const& digest) const;

    /// Installs @p bytes under @p digest, and converges on the byte budget once
    /// enough has been written since the last pass to be worth a walk.
    void store(utility::Sha256Digest const& digest, std::span<std::byte const> bytes) const;

    std::filesystem::path entryPath(utility::Sha256Digest const& digest) const;

  private:
    /// Records @p byteLength as written and answers whether a pass is now due.
    bool accumulateWrite(std::size_t byteLength) const;

    void converge() const;
    void touch(std::filesystem::path const& path) const;

    Config _config;

    /// Seeded so the first write of a process converges: a directory left over
    /// budget by an earlier run is reclaimed then, and amortization governs only
    /// the writes that follow it.
    mutable std::atomic<std::size_t> _unconvergedBytes;
  };
} // namespace ao::rt
