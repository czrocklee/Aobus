// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/library/LibraryUri.h>

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::library
{
  namespace
  {
    constexpr auto kC0ControlLimit = static_cast<unsigned char>(0x20);
    constexpr auto kDeleteControl = static_cast<unsigned char>(0x7F);

    bool hasWindowsRootName(std::string_view text) noexcept
    {
      return text.size() >= 2U && std::isalpha(static_cast<unsigned char>(text[0])) != 0 && text[1] == ':';
    }

    bool escapesRoot(std::filesystem::path const& relative) noexcept
    {
      if (relative.empty() || relative == "." || relative.is_absolute())
      {
        return true;
      }

      return std::ranges::any_of(relative, [](auto const& component) { return component == ".."; });
    }

    Result<> rejectUnresolvedSymlinks(std::filesystem::path const& root,
                                      std::filesystem::path const& relative,
                                      std::string_view const uri)
    {
      auto current = root;

      for (auto const& component : relative)
      {
        current /= component;
        auto ec = std::error_code{};
        auto const status = std::filesystem::symlink_status(current, ec);

        if (ec == std::errc::no_such_file_or_directory)
        {
          return {};
        }

        if (ec)
        {
          return makeError(Error::Code::IoError,
                           std::format("Failed to inspect library path '{}': {}", current.string(), ec.message()));
        }

        if (status.type() == std::filesystem::file_type::not_found)
        {
          return {};
        }

        if (!std::filesystem::is_symlink(status))
        {
          continue;
        }

        current = std::filesystem::canonical(current, ec);

        if (ec)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("Library URI '{}' contains an unresolved symlink component", uri));
        }
      }

      return {};
    }
  } // namespace

  Result<LibraryUri> LibraryUri::parse(std::string_view text)
  {
    if (text.empty())
    {
      return makeError(Error::Code::InvalidInput, "Library URI must be a non-empty path");
    }

    if (std::ranges::any_of(text,
                            [](char const value)
                            {
                              auto const byte = static_cast<unsigned char>(value);
                              return byte < kC0ControlLimit || byte == kDeleteControl;
                            }))
    {
      return makeError(Error::Code::InvalidInput, "Library URI must not contain control characters");
    }

    if (text.size() > kMaxLength)
    {
      return makeError(Error::Code::ValueTooLarge,
                       std::format("Library URI length {} exceeds the maximum of {} bytes", text.size(), kMaxLength));
    }

    auto portable = std::string{text};
    std::ranges::replace(portable, '\\', '/');

    if (portable.starts_with('/') || hasWindowsRootName(portable))
    {
      return makeError(Error::Code::InvalidInput, std::format("Library URI '{}' must be root-relative", text));
    }

    auto path = std::filesystem::path{portable};

    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
    {
      return makeError(Error::Code::InvalidInput, std::format("Library URI '{}' must be root-relative", text));
    }

    path = path.lexically_normal();

    if (!path.has_filename())
    {
      path = path.parent_path();
    }

    if (path.empty() || path == ".")
    {
      return makeError(Error::Code::InvalidInput, "Library URI must name an item below the library root");
    }

    for (auto const& component : path)
    {
      if (component == "..")
      {
        return makeError(Error::Code::InvalidInput, std::format("Library URI '{}' escapes the library root", text));
      }
    }

    auto value = path.generic_string();

    if (value.empty() || value.starts_with('/'))
    {
      return makeError(Error::Code::InvalidInput, std::format("Library URI '{}' must be root-relative", text));
    }

    return LibraryUri{std::move(value)};
  }

  Result<std::filesystem::path> LibraryUri::resolveUnder(std::filesystem::path const& root) const
  {
    auto ec = std::error_code{};
    auto const resolvedRoot = std::filesystem::weakly_canonical(root, ec);

    if (ec)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to resolve library root '{}': {}", root.string(), ec.message()));
    }

    if (auto const symlinkResult = rejectUnresolvedSymlinks(resolvedRoot, std::filesystem::path{_value}, _value);
        !symlinkResult)
    {
      return std::unexpected{symlinkResult.error()};
    }

    auto resolvedPath = std::filesystem::weakly_canonical(resolvedRoot / _value, ec);

    if (ec)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to resolve library path '{}': {}", _value, ec.message()));
    }

    auto const relative = resolvedPath.lexically_relative(resolvedRoot);

    if (escapesRoot(relative))
    {
      return makeError(
        Error::Code::InvalidInput, std::format("Library URI '{}' resolves outside the library root", _value));
    }

    return resolvedPath;
  }
} // namespace ao::library
