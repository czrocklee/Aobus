// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::test
{
  template<typename T>
  T requireValue(Result<T>&& result)
  {
    REQUIRE(result);
    auto value = *std::move(result);
    return value;
  }

  template<typename T>
  T const& requireValue(Result<T> const& result)
  {
    REQUIRE(result);
    return *result;
  }

  /**
   * RAII temporary directory for test files.
   */
  class [[nodiscard]] TempDir final
  {
  public:
    TempDir()
    {
      std::string tmpl = (std::filesystem::temp_directory_path() / "ao_test_XXXXXX").string();

      char const* const result = ::mkdtemp(tmpl.data());

      if (result == nullptr)
      {
        throwException<Exception>("mkdtemp failed");
      }

      _path = result;
    }

    ~TempDir()
    {
      auto ec = std::error_code{};
      std::filesystem::remove_all(_path, ec);
    }

    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;

    TempDir(TempDir&&) = default;
    TempDir& operator=(TempDir&&) = default;

    std::filesystem::path const& path() const { return _path; }

  private:
    std::filesystem::path _path;
  };

  struct [[nodiscard]] TempFile final
  {
    std::filesystem::path path;

    explicit TempFile(std::string_view ext)
    {
      path = std::filesystem::temp_directory_path() / ("ao_test" + std::string{ext});

      auto ofs = std::ofstream{path, std::ios::binary};
      ofs << "dummy content";
    }

    explicit TempFile(std::span<std::uint8_t const> data, std::string_view ext = ".bin")
    {
      path = std::filesystem::temp_directory_path() / ("ao_test" + std::string{ext});

      auto ofs = std::ofstream{path, std::ios::binary};
      ofs.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    ~TempFile() noexcept
    {
      auto ec = std::error_code{};
      std::filesystem::remove(path, ec);
    }

    TempFile(TempFile const&) = delete;
    TempFile& operator=(TempFile const&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
  };

  inline std::string readFile(std::filesystem::path const& path)
  {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
  }
} // namespace ao::test
