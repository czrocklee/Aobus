// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

// LMDB native handle, kept opaque so <lmdb.h> stays out of public headers (it
// otherwise bleeds into 40+ translation units). The real definition is pulled in
// by the lmdb wrapper .cpp files.
struct MDB_env;

namespace ao::lmdb
{
  // Mirror of LMDB's native integer typedefs so callers need not include
  // <lmdb.h>. Equivalence with MDB_dbi / mdb_mode_t is asserted in the .cpp.
  using DbiHandle = unsigned int; // == MDB_dbi
#ifdef _MSC_VER
  using EnvMode = int; // == mdb_mode_t
#elifdef __APPLE__
  using EnvMode = unsigned short; // == mdb_mode_t; Darwin's mode_t is __uint16_t
#else
  using EnvMode = unsigned int; // == mdb_mode_t
#endif

  // Mirrors of the env flags consumers configure. The values are verified
  // against the real macros in Environment.cpp.
  inline constexpr std::uint32_t kEnvNoTls = 0x200000U;
  /**
   * Opens for reading only, mapping whatever the database already holds.
   *
   * No capacity rule applies, because raising a map is a commit such an
   * environment cannot make, and the data file is left exactly as it is: an
   * absent one is not created and no write permission on it is requested.
   *
   * This is not a promise that nothing is written. LMDB still maintains the
   * reader table in the lock file, except on a read-only filesystem where it
   * uses no locks at all.
   */
  inline constexpr std::uint32_t kEnvReadOnly = 0x20000U;
  /**
   * Commits without flushing to stable storage.
   *
   * A committed transaction still survives a process crash. An operating-system
   * crash or power loss can lose the most recent commits or corrupt the
   * environment: LMDB then keeps its integrity only on a filesystem that
   * preserves write order, which nothing here establishes. Use it only for
   * disposable environments, such as test fixtures, where the per-commit flush
   * (write-through on Windows, `F_FULLFSYNC` on macOS) dominates the cost of a
   * small commit.
   */
  inline constexpr std::uint32_t kEnvNoSync = 0x10000U;

  constexpr EnvMode kDefaultEnvironmentMode = 0644;

  /**
   * @brief How much an environment may hold, and how far it has already grown.
   */
  struct EnvironmentCapacity final
  {
    /// Bytes the environment may occupy before a mutation runs out of room.
    std::uint64_t mapBytes = 0;
    /**
     * Bytes covered by the highest page the environment has ever committed.
     *
     * This is a peak rather than a measure of live data. Deleting records
     * returns their pages to the free list for reuse without lowering it, and
     * nothing lowers it in place, so it answers "how much of the map has been
     * needed" rather than "how much is stored".
     */
    std::uint64_t highWaterBytes = 0;
    /// Bytes in one database page.
    std::uint32_t pageBytes = 0;
  };

  /**
   * @brief How far an environment's map may be raised while it opens.
   *
   * The map is chosen at the open boundary, before the environment is handed to
   * a caller, because that is the only point where no transaction and no reader
   * can be holding the old mapping. A live environment is never resized.
   *
   * A default policy changes nothing, so an environment that wants a fixed map
   * can keep asking for one through `Options::pinnedMapBytes` alone.
   */
  struct CapacityPolicy final
  {
    /**
     * Smallest map to open with.
     *
     * Whatever smaller size the database persisted, the floor wins. It can be
     * generous because the map reserves address space rather than disk: the
     * data file grows with the pages transactions use, never with the map.
     */
    std::uint64_t minimumMapBytes = 0;
    /**
     * Largest map this boundary may choose.
     *
     * Zero disables the growth rule only: no step is ever taken and no headroom
     * is reserved. A floor above the current map still applies, because a floor
     * is what the caller asked to open with rather than a reaction to how full
     * the database has become.
     */
    std::uint64_t maximumMapBytes = 0;
  };

  class Environment final
  {
  public:
    struct Options
    {
      /**
       * Environment flags, restricted to `kEnvNoTls`, `kEnvReadOnly`, and `kEnvNoSync`.
       *
       * Opening rejects anything else, because this adapter's contract depends
       * on what the other flags leave alone: `MDB_NOSUBDIR` makes the path the
       * data file rather than its directory, and `MDB_WRITEMAP` sets the data
       * file's length to the map size on POSIX and lets a full volume surface as
       * a mapping fault instead of a returned error. Refusing an unrecognized
       * flag keeps those assumptions a checked fact.
       */
      std::uint32_t flags = 0;
      EnvMode mode = kDefaultEnvironmentMode;
      DbiHandle maxDatabases = 0;
      std::uint32_t maxReaders = 0;
      /**
       * Map size to pin before opening, bypassing what the database persisted.
       *
       * Zero instead lets LMDB adopt the persisted size, which is what allows a
       * database to keep capacity an earlier session gave it. Prefer zero with a
       * `capacity` policy; a nonzero value is for callers that need one known
       * map, such as tests that must reach it.
       *
       * It forbids policy growth but is not a guaranteed exact size: LMDB
       * silently raises a request below the space the environment has already
       * consumed, so the effective map is never under the committed extent.
       */
      std::uint64_t pinnedMapBytes = 0;
      /// Growth applied after opening. Ignored for a read-only environment.
      CapacityPolicy capacity{};
    };

    /**
     * @brief Opens the environment whose directory is @p path.
     *
     * Takes a native path rather than an encoded string and converts it itself,
     * because LMDB decodes what it is handed as UTF-8. A caller that converted
     * first could get a platform's narrow encoding instead, which on Windows
     * need not be UTF-8.
     */
    static Result<Environment> open(std::filesystem::path const& path);
    static Result<Environment> open(std::filesystem::path const& path, Options const& options);

    Environment(Environment const&) = delete;
    Environment& operator=(Environment const&) = delete;

    Environment(Environment&& other) noexcept;
    Environment& operator=(Environment&& other) noexcept;

    ~Environment() noexcept;

    EnvironmentCapacity capacity() const;

    /// The longest key LMDB accepts, derived from the page size the environment was created with.
    std::size_t maxKeySize() const noexcept;

  private:
    struct MdbEnvDeleter final
    {
      void operator()(MDB_env* env) const noexcept;
    };

    using EnvPtr = std::unique_ptr<MDB_env, MdbEnvDeleter>;

    explicit Environment(EnvPtr envPtr) noexcept;

    MDB_env* handle() const noexcept { return _envPtr.get(); }

    EnvPtr _envPtr;

    friend class ReadTransaction;
    friend class WriteTransaction;
  };
} // namespace ao::lmdb
