// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  inline std::filesystem::path requireAudioFixture(std::string_view fileName)
  {
    auto const path = std::filesystem::path{AUDIO_TEST_DATA_DIR} / fileName;

    if (!std::filesystem::exists(path))
    {
      SKIP("Required audio fixture missing: " << path);
    }

    return path;
  }

  inline std::string installAudioFixture(std::filesystem::path const& libraryRoot,
                                         std::string_view const fileName,
                                         std::string_view const libraryUri)
  {
    auto const relativePath = std::filesystem::path{libraryUri};
    REQUIRE(relativePath.is_relative());

    auto const destination = libraryRoot / relativePath;
    std::filesystem::create_directories(destination.parent_path());
    REQUIRE(std::filesystem::is_directory(destination.parent_path()));
    REQUIRE(std::filesystem::copy_file(
      requireAudioFixture(fileName), destination, std::filesystem::copy_options::overwrite_existing));
    return relativePath.generic_string();
  }

  inline std::vector<std::uint8_t> readFileBytes(std::filesystem::path const& path)
  {
    auto ifs = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator{ifs}, std::istreambuf_iterator<char>{}};
  }
} // namespace ao::audio::test
