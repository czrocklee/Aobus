/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <boost/asio/buffer.hpp>

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
  [[nodiscard]] inline boost::asio::const_buffer makeBuffer(const std::vector<char>& data)
  {
    return boost::asio::const_buffer{data.data(), data.size()};
  }

  /**
   * Create a mutable buffer from a vector of bytes.
   */
  [[nodiscard]] inline boost::asio::mutable_buffer makeMutableBuffer(std::vector<char>& data)
  {
    return boost::asio::mutable_buffer{data.data(), data.size()};
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
