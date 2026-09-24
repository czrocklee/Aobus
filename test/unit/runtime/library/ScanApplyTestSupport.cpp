// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/library/ScanApplyTestSupport.h"

#include "runtime/library/ScanApplyOperation.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryScan.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace ao::rt::test
{
  void replaceFile(std::filesystem::path const& target, std::filesystem::path const& source)
  {
    auto const previousTime = std::filesystem::last_write_time(target);
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::last_write_time(target, previousTime + std::chrono::seconds{10});
  }

  TrackId importOne(library::MusicLibrary& library)
  {
    auto plan = LibraryScan{library}.buildPlan().value();
    auto res = ScanApplyOperation{library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    REQUIRE(res->insertedIds.size() == 1);
    return res->insertedIds.front();
  }

  void requirePrepared(ScanApplyOperation& operation)
  {
    auto const prepareRes = operation.prepare();
    REQUIRE(prepareRes);
    REQUIRE(prepareRes->failureCount == 0);
  }

  void requireRevalidation(ScanApplyOperation& operation,
                           std::int32_t const staleCount,
                           std::int32_t const failureCount)
  {
    auto const revalidationRes = operation.revalidatePreparedFiles();
    REQUIRE(revalidationRes);
    REQUIRE(revalidationRes->staleCount == staleCount);
    REQUIRE(revalidationRes->failureCount == failureCount);
    REQUIRE(operation.isReadyForMutation() == (failureCount == 0));
  }
} // namespace ao::rt::test
