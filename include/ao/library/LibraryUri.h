// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace ao::library
{
  /**
   * Canonical, non-empty path relative to a music-library root.
   */
  class LibraryUri final
  {
  public:
    static constexpr std::size_t kMaxLength = 500;

    static Result<LibraryUri> parse(std::string_view text);

    std::string_view value() const noexcept { return _value; }

    /**
     * Resolves the URI below root, including existing symlink components. The
     * root and a non-symlink destination suffix may be absent. A destination
     * outside root or an unresolved symlink is rejected.
     */
    Result<std::filesystem::path> resolveUnder(std::filesystem::path const& root) const;

    bool operator==(LibraryUri const&) const = default;

  private:
    explicit LibraryUri(std::string value)
      : _value{std::move(value)}
    {
    }

    std::string _value;
  };
} // namespace ao::library
