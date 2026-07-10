// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/utility/AtomicFile.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
namespace
{
  std::filesystem::path extendedWindowsPath(std::filesystem::path const& path)
  {
    return std::filesystem::path{L"\\\\?\\" + std::filesystem::absolute(path).wstring()};
  }
} // namespace
#endif

namespace ao::utility::test
{
  TEST_CASE("AtomicFile - writes data atomically with owner-only permissions", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = std::filesystem::path{tempDir.path()} / "config.yaml";

    auto const result = writeAtomically(targetPath, "version: 1\n");
    CHECK(result.has_value());

    REQUIRE(std::filesystem::exists(targetPath));

    auto in = std::ifstream{targetPath};
    auto const content = std::string{std::istreambuf_iterator{in}, std::istreambuf_iterator<char>{}};
    CHECK(content == "version: 1\n");

#ifndef _WIN32
    auto const perms = std::filesystem::status(targetPath).permissions();
    CHECK((perms & std::filesystem::perms::group_read) == std::filesystem::perms::none);
    CHECK((perms & std::filesystem::perms::others_read) == std::filesystem::perms::none);
#endif
  }

  TEST_CASE("AtomicFile - overwrites existing file", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = std::filesystem::path{tempDir.path()} / "state.yaml";

    CHECK(writeAtomically(targetPath, "old").has_value());
    CHECK(writeAtomically(targetPath, "new").has_value());

    auto in = std::ifstream{targetPath};
    auto const content = std::string{std::istreambuf_iterator{in}, std::istreambuf_iterator<char>{}};
    CHECK(content == "new");
  }

  TEST_CASE("AtomicFile - fails when parent directory is not writable", "[utility][unit][atomicfile]")
  {
#ifdef _WIN32
    SKIP("std::filesystem permissions do not make directories unwritable on Windows");
#else
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
#endif
  }

  TEST_CASE("AtomicFile - fails to overwrite a directory", "[utility][unit][atomicfile]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const targetPath = std::filesystem::path{tempDir.path()} / "dir";
    std::filesystem::create_directories(targetPath);

    auto const result = writeAtomically(targetPath, "content");
    CHECK_FALSE(result.has_value());
  }

#ifdef _WIN32

  TEST_CASE("AtomicFile - supports Windows paths beyond MAX_PATH", "[utility][unit][atomicfile][windows]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto targetPath = tempDir.path();

    for (std::int32_t index = 0; index < 6; ++index)
    {
      targetPath /= std::string(48, static_cast<char>('a' + index));
    }

    targetPath /= "state.yaml";
    REQUIRE(targetPath.wstring().size() > 260);

    auto const result = writeAtomically(targetPath, "long path content");
    auto const errorMessage = result ? std::string{} : result.error().message;
    INFO(errorMessage);
    REQUIRE(result);

    auto input = std::ifstream{extendedWindowsPath(targetPath), std::ios::binary};
    auto const content = std::string{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
    CHECK(content == "long path content");
    input.close();

    auto ec = std::error_code{};
    std::filesystem::remove_all(extendedWindowsPath(tempDir.path()), ec);
    CHECK_FALSE(ec);
  }

  TEST_CASE("AtomicFile - concurrent Windows writers use distinct temp files", "[utility][unit][atomicfile][windows]")
  {
    constexpr std::size_t kWriterCount = 8;
    auto const tempDir = ao::test::TempDir{};
    auto targets = std::array<std::filesystem::path, kWriterCount>{};
    auto contents = std::array<std::string, kWriterCount>{};
    auto succeeded = std::array<bool, kWriterCount>{};
    auto writers = std::vector<std::jthread>{};
    writers.reserve(kWriterCount);

    for (std::size_t index = 0; index < kWriterCount; ++index)
    {
      contents[index] = "writer-" + std::to_string(index);
      targets[index] = tempDir.path() / ("state-" + std::to_string(index) + ".yaml");
      writers.emplace_back([&, index]
                           { succeeded[index] = writeAtomically(targets[index], contents[index]).has_value(); });
    }

    writers.clear();

    for (auto const result : succeeded)
    {
      CHECK(result);
    }

    for (std::size_t index = 0; index < kWriterCount; ++index)
    {
      auto input = std::ifstream{targets[index], std::ios::binary};
      auto const content = std::string{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
      CHECK(content == contents[index]);
    }

    for (auto const& entry : std::filesystem::directory_iterator{tempDir.path()})
    {
      CHECK_FALSE(entry.path().filename().string().starts_with(".ao.tmp."));
    }
  }
#endif
} // namespace ao::utility::test
