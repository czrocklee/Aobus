// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/utility/AtomicFile.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace ao::utility::test
{
  TEST_CASE("AtomicFile writes data atomically with owner-only permissions", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = std::filesystem::path{tempDir.path()} / "config.yaml";

    auto const result = writeAtomically(targetPath, "version: 1\n");
    CHECK(result.has_value());

    REQUIRE(std::filesystem::exists(targetPath));

    auto in = std::ifstream{targetPath};
    auto const content = std::string{std::istreambuf_iterator{in}, std::istreambuf_iterator<char>{}};
    CHECK(content == "version: 1\n");

    auto const perms = std::filesystem::status(targetPath).permissions();
    CHECK((perms & std::filesystem::perms::group_read) == std::filesystem::perms::none);
    CHECK((perms & std::filesystem::perms::others_read) == std::filesystem::perms::none);
  }

  TEST_CASE("AtomicFile overwrites existing file", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = std::filesystem::path{tempDir.path()} / "state.yaml";

    CHECK(writeAtomically(targetPath, "old").has_value());
    CHECK(writeAtomically(targetPath, "new").has_value());

    auto in = std::ifstream{targetPath};
    auto const content = std::string{std::istreambuf_iterator{in}, std::istreambuf_iterator<char>{}};
    CHECK(content == "new");
  }

  TEST_CASE("AtomicFile fails when parent directory is not writable", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const readonlyDir = std::filesystem::path{tempDir.path()} / "readonly";
    std::filesystem::create_directories(readonlyDir);
    std::filesystem::permissions(readonlyDir,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace);

    auto const targetPath = readonlyDir / "config.yaml";
    auto const result = writeAtomically(targetPath, "version: 1\n");
    CHECK_FALSE(result.has_value());

    std::filesystem::permissions(
      readonlyDir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  }

  TEST_CASE("AtomicFile fails to overwrite a directory", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = std::filesystem::path{tempDir.path()} / "dir";
    std::filesystem::create_directories(targetPath);

    auto const result = writeAtomically(targetPath, "content");
    CHECK_FALSE(result.has_value());
  }
} // namespace ao::utility::test
