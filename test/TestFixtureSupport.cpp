// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestFixtureSupport.h"

#ifdef AOBUS_BAZEL_TEST_RUNFILES
#include <rules_cc/cc/runfiles/runfiles.h>
#elifndef AOBUS_TEST_SOURCE_DIR
#error "CMake test support requires AOBUS_TEST_SOURCE_DIR"
#endif

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#ifdef AOBUS_BAZEL_TEST_RUNFILES
#include <cstdlib>
#include <memory>
#endif

namespace ao::test
{
  TempDir::TempDir()
  {
    // macOS reports /var as the temporary root even though it resolves through
    // the /private/var symlink. Keep fixture paths in their physical form so
    // production canonicalization does not change their identity mid-test.
    auto const root = std::filesystem::canonical(std::filesystem::temp_directory_path());
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

    throw std::runtime_error{"failed to create temporary test directory"};
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
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  }

#ifdef AOBUS_BAZEL_TEST_RUNFILES
  std::string resolveBazelTestRunfile(std::string_view relativePath)
  {
    using rules_cc::cc::runfiles::Runfiles;
    auto error = std::string{};
    auto runfilesPtr = std::unique_ptr<Runfiles>{Runfiles::CreateForTest(BAZEL_CURRENT_REPOSITORY, &error)};

    if (!runfilesPtr)
    {
      throw std::runtime_error{"Cannot initialize Bazel test runfiles: " + error};
    }

    auto const* const workspace = std::getenv("TEST_WORKSPACE");

    if (workspace == nullptr || *workspace == '\0')
    {
      throw std::runtime_error{"Bazel test runfiles require a nonempty TEST_WORKSPACE"};
    }

    return runfilesPtr->Rlocation(std::string{workspace} + "/" + std::string{relativePath});
  }
#endif

  std::filesystem::path testInputPath(std::string_view const workspaceRelativePath)
  {
#ifdef AOBUS_BAZEL_TEST_RUNFILES
    return std::filesystem::path{resolveBazelTestRunfile(workspaceRelativePath)};
#else
    return std::filesystem::path{AOBUS_TEST_SOURCE_DIR} / std::filesystem::path{std::string{workspaceRelativePath}};
#endif
  }
} // namespace ao::test
