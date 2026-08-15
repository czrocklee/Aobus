// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/lmdb/Environment.h>

#include "detail/EnvironmentDataFile.h"
#include "detail/MapCapacityPolicy.h"
#include "detail/ResultError.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <type_traits>
#include <utility>

namespace ao::lmdb
{
  // The public header mirrors these native LMDB types without including <lmdb.h>;
  // assert the mirrors stay in sync with the real ABI.
  static_assert(std::is_same_v<DbiHandle, ::MDB_dbi>);
  static_assert(std::is_same_v<EnvMode, ::mdb_mode_t>);
  static_assert(kEnvNoTls == MDB_NOTLS);
  static_assert(kEnvReadOnly == MDB_RDONLY);
  // Capacity figures are std::uint64_t so a map size means the same thing
  // everywhere, while LMDB takes std::size_t. The two casts below are only
  // lossless where that holds, and a host where it does not could not address a
  // map this large anyway.
  static_assert(sizeof(std::size_t) >= sizeof(std::uint64_t));

  namespace
  {
    EnvironmentCapacity capacityOf(::MDB_env* const handle)
    {
      auto info = ::MDB_envinfo{};
      auto pageStat = ::MDB_stat{};
      // Both reject only a null environment, which an owned handle cannot be, so
      // the calls stay outside the contract conditions that check their results.
      auto const infoCode = ::mdb_env_info(handle, &info);
      auto const statCode = ::mdb_env_stat(handle, &pageStat);
      AO_INVARIANT(infoCode == MDB_SUCCESS, "mdb_env_info rejected a live environment: {}", ::mdb_strerror(infoCode));
      AO_INVARIANT(statCode == MDB_SUCCESS, "mdb_env_stat rejected a live environment: {}", ::mdb_strerror(statCode));

      return EnvironmentCapacity{
        .mapBytes = static_cast<std::uint64_t>(info.me_mapsize),
        // last_pgno is the highest page index, so the page count is one more.
        .highWaterBytes = (static_cast<std::uint64_t>(info.me_last_pgno) + 1U) * pageStat.ms_psize,
        .pageBytes = pageStat.ms_psize,
      };
    }

    /**
     * Raises the map of a just-opened environment to what the policy asks for.
     *
     * mdb_env_set_mapsize unmaps and remaps the file, invalidating every pointer
     * a live transaction holds, and LMDB documents that it does not check for
     * active transactions itself: the caller must guarantee there are none. What
     * guarantees it here is construction order rather than any check. The handle
     * has not left `open()`, so no caller can hold it and no transaction on this
     * environment can exist yet.
     *
     * The raise takes effect for this process at once but reaches the database
     * only when a later write transaction commits something, so an open alone
     * does not record it.
     */
    Result<> applyCapacityPolicy(::MDB_env* const handle, MapAllocation const allocation, CapacityPolicy const& policy)
    {
      auto const capacity = capacityOf(handle);
      auto const planned = detail::plannedMapBytes(capacity, allocation, policy);

      if (planned <= capacity.mapBytes)
      {
        return {};
      }

      return resultFromCode("mdb_env_set_mapsize", ::mdb_env_set_mapsize(handle, static_cast<std::size_t>(planned)));
    }

    /**
     * Rejects a flag the data-file preparation is not written for.
     *
     * The preparation assumes the environment directory holds `data.mdb` and
     * that the mapping never extends the file. MDB_NOSUBDIR breaks the first and
     * MDB_WRITEMAP the second, and both would do it silently, so an unrecognized
     * flag is refused rather than trusted.
     */
    Result<> admitFlags(std::uint32_t const flags)
    {
      constexpr auto kSupported = kEnvNoTls | kEnvReadOnly;

      if ((flags & ~kSupported) != 0)
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Unsupported LMDB environment flags {:#x}", flags & ~kSupported));
      }

      return {};
    }
  } // namespace

  void Environment::MdbEnvDeleter::operator()(MDB_env* env) const noexcept
  {
    ::mdb_env_close(env);
  }

  Result<Environment> Environment::open(std::filesystem::path const& path)
  {
    return open(path, Options{});
  }

  Result<Environment> Environment::open(std::filesystem::path const& path, Environment::Options const& options)
  {
    if (auto result = admitFlags(options.flags); !result)
    {
      return std::unexpected{result.error()};
    }

    ::MDB_env* handle = nullptr;

    if (auto result = resultFromCode("mdb_env_create", ::mdb_env_create(&handle)); !result)
    {
      return std::unexpected{result.error()};
    }

    auto envPtr = EnvPtr{handle};

    // Only a caller that wants one exact map pins it here. Leaving the size
    // unset is what lets mdb_env_open adopt the size the database persisted,
    // which the capacity policy below may then raise but never lowers.
    if (options.pinnedMapBytes > 0)
    {
      if (auto result =
            resultFromCode("mdb_env_set_mapsize",
                           ::mdb_env_set_mapsize(envPtr.get(), static_cast<std::size_t>(options.pinnedMapBytes)));
          !result)
      {
        return std::unexpected{result.error()};
      }
    }

    if (options.maxDatabases > 0)
    {
      if (auto result = resultFromCode("mdb_env_set_maxdbs", ::mdb_env_set_maxdbs(envPtr.get(), options.maxDatabases));
          !result)
      {
        return std::unexpected{result.error()};
      }
    }

    if (options.maxReaders > 0)
    {
      if (auto result =
            resultFromCode("mdb_env_set_maxreaders", ::mdb_env_set_maxreaders(envPtr.get(), options.maxReaders));
          !result)
      {
        return std::unexpected{result.error()};
      }
    }

    // The data file has to be prepared before LMDB maps it, because the mapping
    // is what fixes the file's length to the map size. Preparation is native
    // filesystem work and takes the path as it is.
    auto const access =
      (options.flags & kEnvReadOnly) != 0 ? detail::DataFileAccess::ReadOnly : detail::DataFileAccess::ReadWrite;
    auto allocationRes = detail::prepareEnvironmentDataFile(path, access);

    if (!allocationRes)
    {
      return std::unexpected{allocationRes.error()};
    }

    // LMDB decodes what it is handed as UTF-8, so the conversion has to say so
    // rather than trust the narrow default a platform applies to a native path.
    auto const nativeUtf8 = utility::pathToUtf8(path);

    if (auto result =
          resultFromCode("mdb_env_open", ::mdb_env_open(envPtr.get(), nativeUtf8.c_str(), options.flags, options.mode));
        !result)
    {
      return std::unexpected{result.error()};
    }

    // A read-only environment maps whatever already exists and could not commit
    // a larger size anyway, so capacity management has nothing to offer it.
    if ((options.flags & kEnvReadOnly) == 0)
    {
      if (auto result = applyCapacityPolicy(envPtr.get(), *allocationRes, options.capacity); !result)
      {
        return std::unexpected{result.error()};
      }
    }

    return Environment{std::move(envPtr), *allocationRes};
  }

  EnvironmentCapacity Environment::capacity() const
  {
    return capacityOf(handle());
  }

  Environment::Environment(EnvPtr envPtr, MapAllocation const mapAllocation) noexcept
    : _envPtr{std::move(envPtr)}, _mapAllocation{mapAllocation}
  {
  }

  Environment::Environment(Environment&& other) noexcept = default;

  Environment& Environment::operator=(Environment&& other) noexcept = default;

  Environment::~Environment() noexcept = default;
} // namespace ao::lmdb
