// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
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
} // namespace ao::test
