// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <ao/utility/ByteView.h>

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
    std::string tmpl = (std::filesystem::temp_directory_path() / "rs_lmdb_test_XXXXXX").string();
    char* result = mkdtemp(tmpl.data());
    if (result == nullptr)
    {
      throw std::runtime_error("mkdtemp failed");
    }

    _path = result;
  }

  ~TempDir()
  {
    std::error_code ec;
    std::filesystem::remove_all(_path, ec);
  }

  TempDir(TempDir const&) = delete;
  TempDir& operator=(TempDir const&) = delete;

  TempDir(TempDir&&) = default;
  TempDir& operator=(TempDir&&) = default;

  std::string path() const { return _path.string(); }

private:
  std::filesystem::path _path;
};

/**
 * Create a vector filled with test data.
 */
inline std::vector<std::byte> createTestData(std::size_t size)
{
  std::vector<std::byte> data(size);
  for (std::size_t i = 0; i < size; ++i)
  {
    data[i] = static_cast<std::byte>(i % 256);
  }

  return data;
}

/**
 * Simple string data for testing.
 */
inline std::vector<std::byte> createStringData(std::string_view str)
{
  auto bytes = ao::utility::bytes::view(str);
  return {bytes.begin(), bytes.end()};
}
