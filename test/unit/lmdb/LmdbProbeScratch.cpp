// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LmdbProbeScratch.h"

#include <filesystem>

#ifdef __APPLE__
#include <lmdb.h>

#include <system_error>
#endif

namespace ao::test
{
#ifdef __APPLE__
  namespace
  {
    void reclaimLockSemaphores(std::filesystem::path const& directory) noexcept
    {
      constexpr ::mdb_mode_t kOwnerReadWrite = 0600;
      ::MDB_env* env = nullptr;

      if (::mdb_env_create(&env) != MDB_SUCCESS)
      {
        return;
      }

      // With every owner gone this open is exclusive, so LMDB unlinks the names
      // the aborted one left before creating its own, and the close, as the last
      // owner, unlinks those.
      [[maybe_unused]] auto const result = ::mdb_env_open(env, directory.c_str(), 0, kOwnerReadWrite);
      ::mdb_env_close(env);
    }
  } // namespace
#endif

  LmdbProbeScratch::~LmdbProbeScratch() noexcept
  {
#ifdef __APPLE__
    try
    {
      auto ec = std::error_code{};
      auto const end = std::filesystem::recursive_directory_iterator{};

      for (auto it = std::filesystem::recursive_directory_iterator{_directory.path(), ec}; !ec && it != end;
           it.increment(ec))
      {
        // ao::lmdb::Environment refuses MDB_NOSUBDIR, so every environment it
        // opens keeps its lock in a lock.mdb beside data.mdb.
        if (it->path().filename() == "lock.mdb")
        {
          reclaimLockSemaphores(it->path().parent_path());
        }
      }
    }
    catch (...) // NOLINT(bugprone-empty-catch) -- Reclamation is best effort; the directory is still removed.
    {
    }
#endif
  }

  std::filesystem::path const& LmdbProbeScratch::path() const
  {
    return _directory.path();
  }
} // namespace ao::test
