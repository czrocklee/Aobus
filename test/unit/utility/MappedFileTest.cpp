// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/MappedFile.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string_view>


namespace ao::utility::test
{
    using namespace ao::utility;

  TEST_CASE("MappedFile - mapping and unmapping", "[utility][unit][mapped_file]")
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

      REQUIRE(mappedFile.isMapped() == false);
      REQUIRE(mappedFile.bytes().empty() == true);

      auto const result = mappedFile.map(testFilePath);
      REQUIRE(result.has_value());
      REQUIRE(mappedFile.isMapped() == true);

      auto const bytes = mappedFile.bytes();
      REQUIRE(bytes.size() == testContent.size());

      auto const mappedStr = std::string_view{reinterpret_cast<char const*>(bytes.data()), bytes.size()};
      REQUIRE(mappedStr == testContent);

      // Unmap
      mappedFile.unmap();
      REQUIRE(mappedFile.isMapped() == false);
      REQUIRE(mappedFile.bytes().empty() == true);
    }

    SECTION("Mapping failure for non-existent file")
    {
      auto mappedFile = MappedFile{};
      auto const result = mappedFile.map(tempDir / "non_existent.bin");

      REQUIRE(!result.has_value());
      REQUIRE(mappedFile.isMapped() == false);
      REQUIRE(mappedFile.bytes().empty() == true);
    }

    std::filesystem::remove_all(tempDir);
  }

} // namespace ao::utility::test