// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <span>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace rs::lmdb::test
{

  /**
   * RAII temporary directory for LMDB test files.
   * Creates a unique temporary directory on construction and
   * removes it on destruction.
   */
  class TempDir
  {
  public:
    TempDir()
    {
      std::random_device rd;
      auto seed = rd();
      auto base = std::filesystem::temp_directory_path() / ("rs_lmdb_test_" + std::to_string(seed));
      _path = base;
      std::filesystem::create_directory(_path);
    }

    ~TempDir()
    {
      std::error_code ec;
      std::filesystem::remove_all(_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    TempDir(TempDir&&) = default;
    TempDir& operator=(TempDir&&) = default;

    [[nodiscard]] std::string path() const { return _path.string(); }
    [[nodiscard]] std::string_view path_view() const { return _path.string(); }

  private:
    std::filesystem::path _path;
  };

  /**
   * Create a test buffer from a vector of bytes.
   */
  [[nodiscard]] inline std::span<std::byte const> makeBuffer(const std::vector<char>& data)
  {
    return std::span<std::byte const>{reinterpret_cast<std::byte const*>(data.data()), data.size()};
  }

  /**
   * Create a mutable buffer from a vector of bytes.
   */
  [[nodiscard]] inline std::span<std::byte> makeMutableBuffer(std::vector<char>& data)
  {
    return std::span<std::byte>{reinterpret_cast<std::byte*>(data.data()), data.size()};
  }

  /**
   * Create a vector filled with test data.
   */
  [[nodiscard]] inline std::vector<char> createTestData(std::size_t size)
  {
    std::vector<char> data(size);
    for (std::size_t i = 0; i < size; ++i)
    {
      data[i] = static_cast<char>(i % 256);
    }
    return data;
  }

  /**
   * Simple string data for testing.
   */
  [[nodiscard]] inline std::vector<char> createStringData(std::string_view str)
  {
    return std::vector<char>{str.begin(), str.end()};
  }

} // namespace rs::lmdb::test
