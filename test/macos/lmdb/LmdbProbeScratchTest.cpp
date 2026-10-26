// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/lmdb/LmdbProbeScratch.h"

#include "test/fatal/ProbeProcess.h"

#include <catch2/catch_test_macros.hpp>
#include <sys/semaphore.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::lmdb::test
{
  namespace
  {
    // LMDB 1.0 derives both semaphore names from one 64-bit id that it records
    // in the lock file after the magic, format, last transaction id and reader
    // count, and spells that id in its own base-85 alphabet behind "/MDBr" or
    // "/MDBw" (mdb_env_mname_init in mdb.c).
    std::vector<std::string> lockFileSemaphoreNames(std::filesystem::path const& lockFilePath)
    {
      constexpr std::streamoff kMutexIdOffset = 24;
      constexpr auto kDigits =
        std::string_view{"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~"};
      constexpr std::size_t kMaximumDigits = 10;

      auto bytes = std::array<char, sizeof(std::uint64_t)>{};
      auto stream = std::ifstream{lockFilePath, std::ios::binary};
      stream.seekg(kMutexIdOffset);
      stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      REQUIRE(stream);

      auto id = std::bit_cast<std::uint64_t>(bytes);
      auto suffix = std::string{};

      while (id != 0 && suffix.size() < kMaximumDigits)
      {
        suffix.push_back(kDigits[id % kDigits.size()]);
        id /= kDigits.size();
      }

      return {"/MDBr" + suffix, "/MDBw" + suffix};
    }

    bool isSemaphoreLinked(std::string const& name)
    {
      auto* const semaphore = ::sem_open(name.c_str(), 0);

      if (semaphore == SEM_FAILED)
      {
        auto const error = errno;
        REQUIRE(error == ENOENT);
        return false;
      }

      ::sem_close(semaphore);
      return true;
    }
  } // namespace

  TEST_CASE("LmdbProbeScratch - reclaims the semaphores of an aborted environment owner")
  {
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
    REQUIRE_FALSE(executablePath.empty());
    auto optScratch = std::optional<ao::test::LmdbProbeScratch>{};
    optScratch.emplace();
    auto const scratchPath = optScratch->path();
    auto const result = ao::test::runProbeProcess(
      executablePath, "lmdb-writer-after-commit:" + scratchPath.filename().string(), std::chrono::seconds{15});
    REQUIRE(result.hasPlatformAbort());

    // The aborted owner left both names allocated after its process exited.
    auto const names = lockFileSemaphoreNames(scratchPath / "lock.mdb");
    REQUIRE(std::ranges::all_of(names, isSemaphoreLinked));

    optScratch.reset();

    CHECK_FALSE(std::filesystem::exists(scratchPath));
    CHECK(std::ranges::none_of(names, isSemaphoreLinked));
  }
} // namespace ao::lmdb::test
