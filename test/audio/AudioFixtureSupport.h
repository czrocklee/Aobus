// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  std::filesystem::path requireAudioFixture(std::string_view fileName);
  std::string installAudioFixture(std::filesystem::path const& libraryRoot,
                                  std::string_view fileName,
                                  std::string_view libraryUri);
  std::vector<std::uint8_t> readFileBytes(std::filesystem::path const& path);
} // namespace ao::audio::test
