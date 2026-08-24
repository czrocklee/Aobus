// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/MappedFile.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string_view>
#include <utility>

namespace ao::utility::test
{
  TEST_CASE("MappedFile - maps files and reports failed mappings", "[utility][unit][mapped-file]")
  {
    auto const tempDir = std::filesystem::temp_directory_path() / "ao_mapped_file_test";
    std::filesystem::create_directories(tempDir);
    auto const testFilePath = tempDir / "test.bin";

    // Create a dummy file for testing
    auto const testContent = std::string_view{"Hello, MappedFile!"};
    {
      auto ofs = std::ofstream{testFilePath, std::ios::binary};
      ofs.write(testContent.data(), static_cast<std::streamsize>(testContent.size()));
    }

    SECTION("Maps successfully and reads correct bytes")
    {
      auto mappedFile = MappedFile{};

      CHECK(mappedFile.isMapped() == false);
      CHECK(mappedFile.bytes().empty() == true);

      auto const result = mappedFile.map(testFilePath);
      CHECK(result.has_value());
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
      auto const result = mappedFile.map(tempDir / "non_existent.bin");

      CHECK(!result.has_value());
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

    std::filesystem::remove_all(tempDir);
  }
} // namespace ao::utility::test
