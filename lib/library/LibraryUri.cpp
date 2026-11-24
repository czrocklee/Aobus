// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/LibraryUri.h>

#include "LibraryUriValidation.h"
#include <ao/Error.h>
#include <ao/utility/FileAccessPath.h>
#include <ao/utility/Path.h>
#include <ao/utility/String.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#endif

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
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
    constexpr auto kUtf8ContinuationMask = static_cast<unsigned char>(0xC0);
    constexpr auto kUtf8ContinuationTag = static_cast<unsigned char>(0x80);

    // Quotes parse input for a diagnostic. Input longer than any URI is cut to
    // that length at a UTF-8 lead byte, so it cannot inflate the message.
    std::string quotedInput(std::string_view const text)
    {
      if (text.size() <= LibraryUri::kMaxLength)
      {
        return std::format("'{}'", text);
      }

      auto end = LibraryUri::kMaxLength;

      while (end > 0 && (static_cast<unsigned char>(text[end]) & kUtf8ContinuationMask) == kUtf8ContinuationTag)
      {
        --end;
      }

      return std::format("'{}...' ({} bytes)", text.substr(0, end), text.size());
    }

    bool hasWindowsRootName(std::string_view text) noexcept
    {
      return text.size() >= 2U && utility::isAsciiAlpha(text[0]) && text[1] == ':';
    }

    bool isNonCanonicalComponent(std::string_view const component) noexcept
    {
      return component.empty() || component == "." || component == "..";
    }

    bool isOutsideRoot(std::filesystem::path const& relative) noexcept
    {
      if (relative.empty() || relative == "." || relative.is_absolute())
      {
        return true;
      }

      return std::ranges::any_of(relative, [](auto const& component) { return component == ".."; });
    }

    // Only the drive designator has case-independent syntax. Never fold
    // directory or filename components, which may be case-sensitive.
    std::filesystem::path relativeToRoot(std::filesystem::path const& path, std::filesystem::path const& root)
    {
#ifdef _WIN32
      auto const& name = path.native();
      auto const& base = root.native();

      auto const upper = [](wchar_t unit)
      { return unit >= L'a' && unit <= L'z' ? static_cast<wchar_t>(unit - L'a' + L'A') : unit; };

      if (name.size() >= 2 && base.size() >= 2 && name[1] == L':' && base[1] == L':' && name[0] != base[0] &&
          upper(name[0]) == upper(base[0]))
      {
        auto adjusted = name;
        adjusted.front() = base.front();
        return utility::pathFromNative(adjusted).lexically_relative(root);
      }
#endif
      return path.lexically_relative(root);
    }

    bool isRedirect(std::filesystem::path const& accessPath, std::error_code& ec)
    {
#ifdef _WIN32
      // symlink_status need not classify a junction as a symlink.
      auto const attributes = ::GetFileAttributesW(accessPath.c_str());

      if (attributes == INVALID_FILE_ATTRIBUTES)
      {
        ec = std::error_code{static_cast<std::int32_t>(::GetLastError()), std::system_category()};
        return false;
      }

      return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
      return std::filesystem::is_symlink(std::filesystem::symlink_status(accessPath, ec));
#endif
    }

    std::unexpected<Error> unresolvedComponent(std::string_view uri, std::error_code ec)
    {
      auto const invalid = ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory ||
                           ec == std::errc::too_many_symbolic_link_levels;
      return makeError(invalid ? Error::Code::InvalidInput : Error::Code::IoError,
                       std::format("Failed to resolve library path component for '{}': {}", uri, ec.message()));
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
        auto const redirects = isRedirect(current, ec);

        if (ec == std::errc::no_such_file_or_directory)
        {
          return {};
        }

        if (ec)
        {
          return makeError(Error::Code::IoError,
                           std::format("Failed to inspect library path '{}': {}",
                                       utility::pathToUtf8(utility::ordinaryFilePath(current)),
                                       ec.message()));
        }

        if (!redirects)
        {
          continue;
        }

        current = std::filesystem::canonical(current, ec);

        if (ec)
        {
          return unresolvedComponent(uri, ec);
        }

        auto accessRes = utility::fileAccessPath(current);

        if (!accessRes)
        {
          return std::unexpected{accessRes.error()};
        }

        current = std::move(*accessRes);
        auto const bound = relativeToRoot(utility::ordinaryFilePath(current), utility::ordinaryFilePath(root));

        if (bound != "." && isOutsideRoot(bound))
        {
          return makeError(
            Error::Code::InvalidInput, std::format("Library URI '{}' resolves outside the library root", uri));
        }
      }

      return {};
    }

    // Keep the input components, not just the final resolved name. Absolute
    // inputs may reach the configured tree through a different symlink or
    // short-name spelling; after the first entry, validate its whole suffix.
    Result<> checkInputTraversal(std::filesystem::path const& root,
                                 std::filesystem::path const& candidate,
                                 std::string_view input)
    {
      auto rootRes = utility::fileAccessPath(root);

      if (!rootRes)
      {
        return std::unexpected{rootRes.error()};
      }

      // The ordinary in-root spelling needs only the redirect checks used for
      // a stored URI. Do not normalize away any step from a disk input.
      if (auto const relative = relativeToRoot(candidate, root);
          !isOutsideRoot(relative) && !std::ranges::contains(relative, std::filesystem::path{"."}))
      {
        return rejectUnresolvedSymlinks(*rootRes, relative, input);
      }

      auto prefix = candidate.root_path();
      auto const parts = candidate.relative_path();
      auto part = parts.begin();
#ifdef _WIN32
      // For UNC paths root_path() names only the server. Start at the share,
      // never probe an incomplete authority or skip the disk root itself.
      if (prefix.native().starts_with(LR"(\\)") && part != parts.end())
      {
        prefix /= *part++;
      }
#endif
      auto diskRootRes = utility::weaklyCanonicalFilePath(prefix);

      if (!diskRootRes)
      {
        return std::unexpected{diskRootRes.error()};
      }

      auto const start = relativeToRoot(*diskRootRes, root);
      bool entered = start == "." || !isOutsideRoot(start);

      while (part != parts.end())
      {
        prefix /= *part++;
        // Keep prefix in its input spelling. Each Windows I/O projects just
        // this prefix, so a later '..' cannot erase an earlier observation.
        auto accessRes = utility::fileAccessPath(prefix);

        if (!accessRes)
        {
          return std::unexpected{accessRes.error()};
        }

        auto ec = std::error_code{};
        auto const redirects = isRedirect(*accessRes, ec);

        if (ec && ec != std::errc::no_such_file_or_directory)
        {
          return makeError(
            Error::Code::IoError,
            std::format("Failed to inspect library path '{}': {}", utility::pathToUtf8(prefix), ec.message()));
        }

#ifndef _WIN32
        if (ec && std::find(part, parts.end(), "..") != parts.end())
        {
          return makeError(Error::Code::NotFound, "Cannot traverse missing directory in track file path");
        }
#endif
        auto resolved =
          redirects ? std::filesystem::canonical(*accessRes, ec) : std::filesystem::weakly_canonical(*accessRes, ec);

        if (ec)
        {
          if (redirects)
          {
            return unresolvedComponent(input, ec);
          }

          return makeError(
            Error::Code::IoError,
            std::format("Failed to resolve library path '{}': {}", utility::pathToUtf8(prefix), ec.message()));
        }

        auto const relative = relativeToRoot(utility::ordinaryFilePath(resolved), root);
        auto const inside = relative == "." || !isOutsideRoot(relative);

        if (!inside && entered)
        {
          return makeError(Error::Code::InvalidInput, std::format("track file is outside music root: {}", input));
        }

        entered = inside;
      }

      if (!entered)
      {
        return makeError(Error::Code::InvalidInput, std::format("track file is outside music root: {}", input));
      }

      return {};
    }
#ifdef _WIN32
    // An absent ordinary suffix is allowed. A failed open of a reachable
    // redirect (or a non-directory ancestor) is not evidence of absence.
    Result<> validateBaselineDirectory(std::filesystem::path directory)
    {
      for (;;)
      {
        auto accessRes = utility::fileAccessPath(directory);

        if (!accessRes)
        {
          return std::unexpected{accessRes.error()};
        }

        directory = utility::ordinaryFilePath(*accessRes);
        auto ec = std::error_code{};

        if (auto const physical = std::filesystem::canonical(*accessRes, ec); !ec)
        {
          auto resolvedRes = utility::fileAccessPath(physical);

          if (!resolvedRes)
          {
            return std::unexpected{resolvedRes.error()};
          }

          if (std::filesystem::is_directory(*resolvedRes, ec))
          {
            return {};
          }

          if (!ec)
          {
            ec = std::make_error_code(std::errc::not_a_directory);
          }
        }
        else if (ec == std::errc::no_such_file_or_directory)
        {
          auto probeEc = std::error_code{};

          if (isRedirect(*accessRes, probeEc))
          {
            return unresolvedComponent(utility::pathToUtf8(directory), ec);
          }

          if (probeEc == std::errc::no_such_file_or_directory)
          {
            if (auto parent = directory.parent_path(); parent != directory)
            {
              directory = std::move(parent);
              continue;
            }
          }
          else if (probeEc)
          {
            ec = probeEc;
          }
        }

        return makeError(
          Error::Code::IoError,
          std::format("Failed to inspect baseline directory '{}': {}", utility::pathToUtf8(directory), ec.message()));
      }
    }
#endif
  } // namespace

  bool detail::isCanonicalLibraryUri(std::string_view const text) noexcept
  {
    if (text.empty() || text.size() > LibraryUri::kMaxLength || hasWindowsRootName(text))
    {
      return false;
    }

    std::size_t componentStart = 0;

    for (std::size_t index = 0; index < text.size(); ++index)
    {
      auto const value = text[index];

      if (auto const byte = static_cast<unsigned char>(value);
          byte < kC0ControlLimit || byte == kDeleteControl || value == '\\')
      {
        return false;
      }

      if (value != '/')
      {
        continue;
      }

      if (isNonCanonicalComponent(text.substr(componentStart, index - componentStart)))
      {
        return false;
      }

      componentStart = index + 1U;
    }

    return !isNonCanonicalComponent(text.substr(componentStart));
  }

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

    auto portable = std::string{text};
    std::ranges::replace(portable, '\\', '/');

    if (portable.starts_with('/') || hasWindowsRootName(portable))
    {
      return makeError(
        Error::Code::InvalidInput, std::format("Library URI {} must be root-relative", quotedInput(text)));
    }

    auto path = utility::pathFromUtf8(portable);

    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
    {
      return makeError(
        Error::Code::InvalidInput, std::format("Library URI {} must be root-relative", quotedInput(text)));
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
        return makeError(
          Error::Code::InvalidInput, std::format("Library URI {} escapes the library root", quotedInput(text)));
      }
    }

    auto value = utility::pathToGenericUtf8(path);

    if (value.empty() || value.starts_with('/') || hasWindowsRootName(value))
    {
      return makeError(
        Error::Code::InvalidInput, std::format("Library URI {} must be root-relative", quotedInput(text)));
    }

    if (value.size() > kMaxLength)
    {
      return makeError(Error::Code::ValueTooLarge,
                       std::format("Library URI length {} exceeds the maximum of {} bytes", value.size(), kMaxLength));
    }

    return LibraryUri{std::move(value)};
  }

  Result<LibraryUri> LibraryUri::fromFilePath(std::filesystem::path const& configuredRoot,
                                              std::filesystem::path const& input)
  {
    auto rootRes = utility::weaklyCanonicalFilePath(configuredRoot);

    if (!rootRes)
    {
      return std::unexpected{rootRes.error()};
    }

    auto const& root = *rootRes;
    auto inputRes = utility::checkedFileSpelling(input);

    if (!inputRes)
    {
      return std::unexpected{inputRes.error()};
    }

    auto const candidate = inputRes->is_absolute() ? *inputRes : root / *inputRes;
    auto candidateRes = utility::fileAccessPath(candidate);

    if (!candidateRes)
    {
      return std::unexpected{candidateRes.error()};
    }

    auto const inputText = utility::pathToUtf8(input);
    auto ec = std::error_code{};
    auto const fullPath = utility::ordinaryFilePath(std::filesystem::weakly_canonical(*candidateRes, ec));
    auto const tooLong = ec == std::errc::filename_too_long;

    if (ec && !tooLong)
    {
      if (ec == std::errc::no_such_file_or_directory)
      {
        return makeError(Error::Code::NotFound, std::format("track file not found under music root: {}", inputText));
      }

      return makeError(
        Error::Code::IoError, std::format("Failed to resolve track file '{}': {}", inputText, ec.message()));
    }

    auto relative = relativeToRoot(fullPath, root);

    if (tooLong)
    {
      // Measure a path the platform cannot resolve under either spelling of
      // the configured root. Its lexical form can diagnose, never admit, it.
      auto const lexical = candidate.lexically_normal();
      relative = relativeToRoot(lexical, root);

      if (isOutsideRoot(relative))
      {
        relative = relativeToRoot(lexical, utility::ordinaryFilePath(configuredRoot).lexically_normal());
      }
    }

    if (isOutsideRoot(relative))
    {
      return makeError(Error::Code::InvalidInput, std::format("track file is outside music root: {}", inputText));
    }

    auto uriRes = parse(utility::pathToGenericUtf8(relative));

    if (!uriRes)
    {
      auto const& error = uriRes.error();
      return makeError(
        error.code,
        std::format("{}: {}: {}",
                    error.code == Error::Code::ValueTooLarge ? "track file path is too long for the library"
                                                             : "track file path does not form a library URI",
                    inputText,
                    error.message));
    }

    if (tooLong)
    {
      return makeError(Error::Code::IoError,
                       std::format("track file path is too long for the platform: {}: {}", inputText, ec.message()));
    }

    if (auto checkRes = checkInputTraversal(root, candidate, inputText); !checkRes)
    {
      return std::unexpected{checkRes.error()};
    }

    return uriRes;
  }

  Result<std::filesystem::path> LibraryUri::resolveUnder(std::filesystem::path const& root) const
  {
    auto fileRes = resolveFileUnder(root);
    return fileRes.transform([](auto const& path) { return utility::ordinaryFilePath(path); });
  }

  Result<std::filesystem::path> LibraryUri::resolveFileUnder(std::filesystem::path const& root) const
  {
    auto rootRes = utility::weaklyCanonicalFilePath(root);

    if (!rootRes)
    {
      return std::unexpected{rootRes.error()};
    }

    auto accessRootRes = utility::fileAccessPath(*rootRes);

    if (!accessRootRes)
    {
      return std::unexpected{accessRootRes.error()};
    }

    auto const relative = utility::pathFromUtf8(_value);
    auto candidateRes = utility::fileAccessPath(*rootRes / relative);

    if (!candidateRes)
    {
      return std::unexpected{candidateRes.error()};
    }

    if (auto const symlinkRes = rejectUnresolvedSymlinks(*accessRootRes, relative, _value); !symlinkRes)
    {
      return std::unexpected{symlinkRes.error()};
    }

    auto ec = std::error_code{};
    auto resolvedPath = utility::ordinaryFilePath(std::filesystem::weakly_canonical(*candidateRes, ec));

    if (ec)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to resolve library path '{}': {}", _value, ec.message()));
    }

    auto const resolvedRelative = relativeToRoot(resolvedPath, *rootRes);

    if (isOutsideRoot(resolvedRelative))
    {
      return makeError(
        Error::Code::InvalidInput, std::format("Library URI '{}' resolves outside the library root", _value));
    }

    return utility::fileAccessPath(resolvedPath);
  }

  Result<std::optional<std::filesystem::path>> LibraryUri::resolveOptionalFileUnder(
    std::filesystem::path const& root) const
  {
#ifdef _WIN32
    auto prefix = std::filesystem::path{};
    bool unrepresentable = false;

    // Validate the complete URI before looking up a baseline. An unavailable
    // name must not hide a later stream, drive specification, or pattern.
    for (auto const& component : utility::pathFromUtf8(_value))
    {
      auto nameRes = utility::isRepresentableFileName(component);

      if (!nameRes)
      {
        return std::unexpected{nameRes.error()};
      }

      if (!*nameRes)
      {
        unrepresentable = true;
      }
      else if (!unrepresentable)
      {
        prefix /= component;
      }
    }

    if (unrepresentable)
    {
      // This URI can never be a native file. That does not excuse an
      // invalid root, an inaccessible ancestor, or a prior redirect outside.
      if (auto checkRes = validateBaselineDirectory(root); !checkRes)
      {
        return std::unexpected{checkRes.error()};
      }

      if (!prefix.empty())
      {
        auto rootRes = utility::weaklyCanonicalFilePath(root);

        if (!rootRes)
        {
          return std::unexpected{rootRes.error()};
        }

        auto accessRes = utility::fileAccessPath(*rootRes);

        if (!accessRes)
        {
          return std::unexpected{accessRes.error()};
        }

        if (auto checkRes = rejectUnresolvedSymlinks(*accessRes, prefix, _value); !checkRes)
        {
          return std::unexpected{checkRes.error()};
        }

        if (auto checkRes = validateBaselineDirectory(*rootRes / prefix); !checkRes)
        {
          return std::unexpected{checkRes.error()};
        }
      }

      return std::nullopt;
    }
#endif
    auto pathRes = resolveFileUnder(root);

    if (!pathRes)
    {
      return std::unexpected{pathRes.error()};
    }

    return std::optional{std::move(*pathRes)};
  }
} // namespace ao::library
