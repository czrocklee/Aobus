// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::test
{
  template<typename T>
  T requireValue(Result<T>&& result)
  {
    REQUIRE(result);
    return *std::move(result);
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
    TempDir();
    ~TempDir() noexcept;

    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;

    TempDir(TempDir&& other) noexcept;
    TempDir& operator=(TempDir&& other) noexcept;

    std::filesystem::path const& path() const;

  private:
    void cleanup() noexcept;

    std::filesystem::path _path;
  };

  // TempDir owns cleanup even though TempFile's destructor can remain defaulted.
  struct [[nodiscard]] TempFile final
  {
  public:
    // Test call sites intentionally expose the fixture path as simple data.
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
    std::filesystem::path path;

    explicit TempFile(std::string_view ext);
    explicit TempFile(std::span<std::uint8_t const> data, std::string_view ext = ".bin");

    ~TempFile() noexcept = default;

    TempFile(TempFile const&) = delete;
    TempFile& operator=(TempFile const&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

  private:
    TempDir _directory;
  };

  std::string readFile(std::filesystem::path const& path);

  template<typename TState>
  struct RenderLog final
  {
    std::vector<TState> states;

    void render(TState state) { states.push_back(std::move(state)); }

    bool empty() const noexcept { return states.empty(); }
    TState const& last() const { return states.back(); }
    void clear() { states.clear(); }
  };
} // namespace ao::test
