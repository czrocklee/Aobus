// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestFixtureSupport.h"

#include <ao/Exception.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <span>
#include <system_error>
#include <utility>

namespace ao::test
{
  TempDir::TempDir()
  {
    auto const root = std::filesystem::temp_directory_path();
    auto device = std::random_device{};
    auto distribution = std::uniform_int_distribution<std::uint64_t>{};

    for (std::uint32_t attempt = 0; attempt < 100; ++attempt)
    {
      auto candidate = root / std::format("ao_test_{:016x}", distribution(device));

      if (auto ec = std::error_code{}; std::filesystem::create_directory(candidate, ec))
      {
        _path = std::move(candidate);
        return;
      }
    }

    throwException<Exception>("failed to create temporary test directory");
  }

  TempDir::~TempDir() noexcept
  {
    cleanup();
  }

  TempDir::TempDir(TempDir&& other) noexcept
    : _path{std::exchange(other._path, {})}
  {
  }

  TempDir& TempDir::operator=(TempDir&& other) noexcept
  {
    _path.swap(other._path);
    return *this;
  }

  std::filesystem::path const& TempDir::path() const
  {
    return _path;
  }

  void TempDir::cleanup() noexcept
  {
    if (_path.empty())
    {
      return;
    }

    auto ec = std::error_code{};

    try
    {
#ifdef _WIN32
      // Extended paths let cleanup traverse test fixtures that intentionally
      // exercise paths beyond MAX_PATH.
      auto const native = std::filesystem::absolute(_path).wstring();
      auto const cleanupPath = native.starts_with(L"\\\\") ? std::filesystem::path{L"\\\\?\\UNC\\" + native.substr(2)}
                                                           : std::filesystem::path{L"\\\\?\\" + native};
      std::filesystem::remove_all(cleanupPath, ec);
#else
      std::filesystem::remove_all(_path, ec);
#endif
    }
    catch (...)
    {
      ec = std::make_error_code(std::errc::io_error);
    }

    if (!ec)
    {
      return;
    }

    try
    {
      auto const pathString = _path.string();
      // NOLINTNEXTLINE(modernize-use-std-print): C I/O cannot throw from this noexcept cleanup path.
      std::fprintf(
        stderr, "Aobus test temporary directory cleanup failed for %s (error %d)\n", pathString.c_str(), ec.value());
    }
    catch (...)
    {
      // NOLINTNEXTLINE(modernize-use-std-print): C I/O is the allocation-free fallback in a catch handler.
      std::fprintf(stderr, "Aobus test temporary directory cleanup failed (error %d)\n", ec.value());
    }
  }

  TempFile::TempFile(std::string_view ext)
  {
    path = _directory.path() / ("file" + std::string{ext});
    auto ofs = std::ofstream{path, std::ios::binary};
    ofs << "dummy content";
  }

  TempFile::TempFile(std::span<std::uint8_t const> data, std::string_view ext)
  {
    path = _directory.path() / ("file" + std::string{ext});
    auto ofs = std::ofstream{path, std::ios::binary};
    ofs.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
  }

  std::string readFile(std::filesystem::path const& path)
  {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
  }
} // namespace ao::test
