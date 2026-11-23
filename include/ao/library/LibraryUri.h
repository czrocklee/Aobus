// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <filesystem>
#include <optional>
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
    // Byte length limit of the canonical value. Zero-padded to a four-byte
    // multiple, it is the manifest key, which must fit LMDB's computed maximum
    // key size of 1,978 bytes for 4 KiB pages, the smallest supported page size.
    static constexpr std::size_t kMaxLength = 1'976;

    static Result<LibraryUri> parse(std::string_view text);

    // Derive the canonical URI of a disk input (absolute or root-relative).
    // Once its spelling enters root, no later component may redirect outside.
    static Result<LibraryUri> fromFilePath(std::filesystem::path const& root, std::filesystem::path const& input);

    std::string_view value() const noexcept { return _value; }

    /**
     * Resolves the URI below root, including existing symlink components. The
     * root and a non-symlink destination suffix may be absent. A destination
     * outside root or an unresolved symlink is rejected.
     */
    Result<std::filesystem::path> resolveUnder(std::filesystem::path const& root) const;

    // Same physical admission, returning the native spelling for disk I/O.
    Result<std::filesystem::path> resolveFileUnder(std::filesystem::path const& root) const;

    // For optional media baselines only. A host-unrepresentable URI filename
    // has no baseline, but only after admitting the root and reachable parent
    // directories. Resolution and inspection failures remain errors.
    Result<std::optional<std::filesystem::path>> resolveOptionalFileUnder(std::filesystem::path const& root) const;

    bool operator==(LibraryUri const&) const = default;

  private:
    explicit LibraryUri(std::string value)
      : _value{std::move(value)}
    {
    }

    std::string _value;
  };
} // namespace ao::library
