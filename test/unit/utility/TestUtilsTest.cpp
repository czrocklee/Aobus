// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"

#include "test/unit/FilesystemTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace ao::test
{
  TEST_CASE("TestUtils - temporary files use isolated directories", "[utility][unit][test-utils]")
  {
    constexpr auto kFirstData = std::to_array<std::uint8_t>({'a'});
    constexpr auto kSecondData = std::to_array<std::uint8_t>({'b'});
    auto firstDirectory = std::filesystem::path{};
    auto secondDirectory = std::filesystem::path{};

    {
      auto const first = TempFile{kFirstData, ".bin"};
      auto const second = TempFile{kSecondData, ".bin"};
      firstDirectory = first.path.parent_path();
      secondDirectory = second.path.parent_path();

      CHECK(first.path != second.path);
      CHECK(firstDirectory != secondDirectory);
      CHECK(readFile(first.path) == "a");
      CHECK(readFile(second.path) == "b");
    }

    CHECK_FALSE(std::filesystem::exists(firstDirectory));
    CHECK_FALSE(std::filesystem::exists(secondDirectory));
  }

  TEST_CASE("TestUtils - moving temporary directories preserves cleanup ownership", "[utility][unit][test-utils]")
  {
    auto firstPath = std::filesystem::path{};
    auto secondPath = std::filesystem::path{};

    {
      auto first = TempDir{};
      auto second = TempDir{};
      firstPath = first.path();
      secondPath = second.path();

      first = std::move(second);

      CHECK(first.path() == secondPath);
      // TempDir move assignment swaps ownership, so its moved-from state is specified here.
      // NOLINTNEXTLINE(bugprone-use-after-move)
      CHECK(second.path() == firstPath);
      CHECK(std::filesystem::exists(firstPath));
      CHECK(std::filesystem::exists(secondPath));
    }

    CHECK_FALSE(std::filesystem::exists(firstPath));
    CHECK_FALSE(std::filesystem::exists(secondPath));
  }

  TEST_CASE("FilesystemTestSupport - read denial restores directory access", "[utility][unit][test-support]")
  {
    auto const temp = TempDir{};
    auto const blocked = temp.path() / "blocked";
    auto const child = blocked / "child.txt";
    std::filesystem::create_directory(blocked);
    std::ofstream{child} << "content";

    {
      auto const denied = ScopedDirectoryAccessGuard{blocked, DeniedDirectoryAccess::Read};

      if (!denied.effective())
      {
        SKIP("the current process bypasses directory read restrictions");
      }

      auto ec = std::error_code{};
      [[maybe_unused]] auto const iterator = std::filesystem::directory_iterator{blocked, ec};
      CHECK(ec == std::errc::permission_denied);

      ec.clear();
      CHECK_FALSE(std::filesystem::exists(child, ec));
      CHECK(ec == std::errc::permission_denied);
    }

    CHECK(std::filesystem::exists(child));
    CHECK(readFile(child) == "content");
  }

  TEST_CASE("FilesystemTestSupport - write denial restores directory access", "[utility][unit][test-support]")
  {
    auto const temp = TempDir{};
    auto const blocked = temp.path() / "blocked";
    auto const child = blocked / "child.txt";
    std::filesystem::create_directory(blocked);

    {
      auto const denied = ScopedDirectoryAccessGuard{blocked, DeniedDirectoryAccess::Write};

      if (!denied.effective())
      {
        SKIP("the current process bypasses directory write restrictions");
      }

      auto output = std::ofstream{child};
      CHECK_FALSE(output);
    }

    auto output = std::ofstream{child};
    REQUIRE(output);
    output << "content";
    output.close();
    CHECK(readFile(child) == "content");
  }
} // namespace ao::test
