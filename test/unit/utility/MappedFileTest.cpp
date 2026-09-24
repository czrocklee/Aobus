// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/MappedFile.h>

#include "test/unit/TestFixtureSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <ios>
#include <string_view>
#include <utility>

namespace ao::utility::test
{
  TEST_CASE("MappedFile - maps files and reports failed mappings", "[utility][unit][mapped-file]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const testFilePath = tempDir.path() / "test.bin";
    auto const testContent = std::string_view{"Hello, MappedFile!"};
    {
      auto ofs = std::ofstream{testFilePath, std::ios::binary};
      REQUIRE(ofs);
      ofs.write(testContent.data(), static_cast<std::streamsize>(testContent.size()));
      REQUIRE(ofs.good());
    }

    SECTION("Maps successfully and reads correct bytes")
    {
      auto mappedFile = MappedFile{};

      CHECK(mappedFile.isMapped() == false);
      CHECK(mappedFile.bytes().empty() == true);

      auto const res = mappedFile.map(testFilePath);
      CHECK(res.has_value());
      CHECK(mappedFile.isMapped() == true);

      auto const bytes = mappedFile.bytes();
      REQUIRE(bytes.size() == testContent.size());

      auto const mappedStr = std::string_view{reinterpret_cast<char const*>(bytes.data()), bytes.size()};
      CHECK(mappedStr == testContent);

      // Unmap
      mappedFile.unmap();
      CHECK(mappedFile.isMapped() == false);
      CHECK(mappedFile.bytes().empty() == true);
    }

    SECTION("Mapping failure for non-existent file")
    {
      auto mappedFile = MappedFile{};
      auto const res = mappedFile.map(tempDir.path() / "non_existent.bin");

      CHECK(!res.has_value());
      CHECK(mappedFile.isMapped() == false);
      CHECK(mappedFile.bytes().empty() == true);
    }

    SECTION("Move construction leaves the source safe and reusable")
    {
      auto source = MappedFile{};
      REQUIRE(source.map(testFilePath));

      auto moved = MappedFile{std::move(source)};
      REQUIRE(moved.isMapped());
      auto const movedBytes = moved.bytes();
      REQUIRE(movedBytes.size() == testContent.size());
      CHECK(std::string_view{reinterpret_cast<char const*>(movedBytes.data()), movedBytes.size()} == testContent);

      // NOLINTNEXTLINE(bugprone-use-after-move): Moved-from behavior is the contract under test.
      CHECK(source.isMapped() == false);
      CHECK(source.bytes().empty() == true);
      source.unmap();
      CHECK(source.isMapped() == false);

      REQUIRE(source.map(testFilePath));
      CHECK(source.isMapped() == true);
      auto const reusedBytes = source.bytes();
      REQUIRE(reusedBytes.size() == testContent.size());
      CHECK(std::string_view{reinterpret_cast<char const*>(reusedBytes.data()), reusedBytes.size()} == testContent);
    }

    SECTION("Move assignment leaves the source safe and reusable")
    {
      auto source = MappedFile{};
      REQUIRE(source.map(testFilePath));
      auto target = MappedFile{};
      REQUIRE(target.map(testFilePath));

      target = std::move(source);
      REQUIRE(target.isMapped());
      auto const targetBytes = target.bytes();
      REQUIRE(targetBytes.size() == testContent.size());
      CHECK(std::string_view{reinterpret_cast<char const*>(targetBytes.data()), targetBytes.size()} == testContent);

      // NOLINTNEXTLINE(bugprone-use-after-move): Moved-from behavior is the contract under test.
      CHECK(source.isMapped() == false);
      CHECK(source.bytes().empty() == true);
      source.unmap();
      CHECK(source.isMapped() == false);

      REQUIRE(source.map(testFilePath));
      CHECK(source.isMapped() == true);
      auto const reusedBytes = source.bytes();
      REQUIRE(reusedBytes.size() == testContent.size());
      CHECK(std::string_view{reinterpret_cast<char const*>(reusedBytes.data()), reusedBytes.size()} == testContent);
    }
  }

  TEST_CASE("MappedFile - a failed remap leaves the previous mapping closed", "[utility][unit][mapped-file]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const path = tempDir.path() / "present.bin";
    {
      auto output = std::ofstream{path, std::ios::binary};
      REQUIRE(output);
      output << "before";
      REQUIRE(output.good());
    }

    auto mappedFile = MappedFile{};
    REQUIRE(mappedFile.map(path));
    REQUIRE(mappedFile.isMapped());

    auto const res = mappedFile.map(tempDir.path() / "missing.bin");
    REQUIRE_FALSE(res);
    CHECK_FALSE(mappedFile.isMapped());
    CHECK(mappedFile.bytes().empty());

    // map() unmaps first; a later successful map must still be possible.
    REQUIRE(mappedFile.map(path));
    auto const bytes = mappedFile.bytes();
    REQUIRE(bytes.size() == 6);
    CHECK(std::string_view{reinterpret_cast<char const*>(bytes.data()), bytes.size()} == "before");
  }
} // namespace ao::utility::test
