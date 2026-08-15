// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/FileAllocation.h>

#include "test/unit/TestFixtureSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

namespace ao::utility::test
{
  namespace
  {
    constexpr std::size_t kWrittenBytes = std::size_t{256} * 1024;

    void writeBytes(std::filesystem::path const& path, std::size_t const count)
    {
      auto output = std::ofstream{path, std::ios::binary};
      auto const block = std::string(count, 'a');
      output.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
  } // namespace

  TEST_CASE("allocatedFileBytes - reports zero for a file that cannot be inspected", "[utility][unit][allocation]")
  {
    auto const temp = ao::test::TempDir{};
    CHECK(allocatedFileBytes(temp.path() / "missing.bin") == 0);
  }

  TEST_CASE("allocatedFileBytes - covers the bytes a dense file stores", "[utility][unit][allocation]")
  {
    auto const temp = ao::test::TempDir{};
    auto const path = temp.path() / "dense.bin";
    writeBytes(path, kWrittenBytes);

    auto const allocated = allocatedFileBytes(path);
    CHECK(allocated >= kWrittenBytes);
    // Allocation rounds up to whole filesystem blocks, never to something wild.
    CHECK(allocated < kWrittenBytes * 2);
  }

  TEST_CASE("allocatedFileBytes - an empty file costs no data blocks", "[utility][unit][allocation]")
  {
    auto const temp = ao::test::TempDir{};
    auto const path = temp.path() / "empty.bin";
    writeBytes(path, 0);

    REQUIRE(std::filesystem::exists(path));
    CHECK(allocatedFileBytes(path) == 0);
  }
} // namespace ao::utility::test
