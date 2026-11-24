// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/FileAccessPath.h>

#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <expected>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#endif

namespace ao::utility
{
#ifdef _WIN32
  namespace
  {
    constexpr std::wstring_view kVerbatim = LR"(\\?\)";
    constexpr std::wstring_view kVerbatimUnc = LR"(\\?\UNC\)";

    bool isWindowsSeparator(wchar_t unit) noexcept
    {
      return unit == L'\\' || unit == L'/';
    }

    bool hasInvalidNamespaceSpelling(std::wstring_view raw, bool verbatim) noexcept
    {
      return raw.size() >= 4 && isWindowsSeparator(raw[0]) && isWindowsSeparator(raw[1]) &&
             isWindowsSeparator(raw[3]) && (raw[2] == L'.' || (raw[2] == L'?' && (!verbatim || raw.contains(L'/'))));
    }

    bool isDrivePath(std::wstring_view text) noexcept
    {
      return text.size() >= 3 && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) &&
             text[1] == L':' && text[2] == L'\\';
    }

    wchar_t upperUnit(wchar_t unit) noexcept
    {
      return unit >= L'a' && unit <= L'z' ? static_cast<wchar_t>(unit - L'a' + L'A') : unit;
    }

    std::wstring upperAscii(std::wstring_view text)
    {
      auto upper = std::wstring{text};
      std::ranges::transform(upper, upper.begin(), upperUnit);
      return upper;
    }

    bool startsWithAsciiToken(std::wstring_view name, std::wstring_view upperToken) noexcept
    {
      return std::ranges::equal(name.substr(0, upperToken.size()), upperToken, {}, upperUnit);
    }

    bool isReservedName(std::wstring_view component)
    {
      auto stem = component.substr(0, component.find(L'.'));

      while (stem.ends_with(L' '))
      {
        stem.remove_suffix(1);
      }

      auto const upper = upperAscii(stem);
      return upper == L"CON" || upper == L"AUX" || upper == L"NUL" || upper == L"PRN" || upper == L"CONIN$" ||
             upper == L"CONOUT$" ||
             (upper.size() == 4 && (upper.starts_with(L"COM") || upper.starts_with(L"LPT")) &&
              std::wstring_view{L"0123456789\u00b9\u00b2\u00b3"}.contains(upper.back()));
    }

    bool hasLiteralNameSyntax(std::wstring_view name)
    {
      return !name.empty() && name.find_first_of(L"<>:\"|?*/\\") == std::wstring_view::npos &&
             std::ranges::none_of(name, [](wchar_t unit) { return unit < L' '; });
    }

    bool isAvailableName(std::wstring_view name)
    {
      return !name.ends_with(L'.') && !name.ends_with(L' ') && !isReservedName(name);
    }

    bool isDiskComponent(std::wstring_view name)
    {
      return hasLiteralNameSyntax(name) && isAvailableName(name);
    }

    // Examine the raw UNC authority before filesystem::path can coalesce
    // separators or reinterpret an incomplete root as drive-relative.
    bool hasDiskShare(std::wstring_view authority)
    {
      auto const separator = authority.find_first_of(L"\\/");

      if (separator == std::wstring_view::npos || !isDiskComponent(authority.substr(0, separator)))
      {
        return false;
      }

      authority.remove_prefix(separator + 1);
      auto const share = authority.substr(0, authority.find_first_of(L"\\/"));
      auto const upper = upperAscii(share);
      return isDiskComponent(share) && upper != L"PIPE" && upper != L"MAILSLOT";
    }
  } // namespace
#endif

  Result<bool> isRepresentableFileName(std::filesystem::path const& component)
  {
    if (component.empty() || component != component.filename() || component == "." || component == ".." ||
        component.native().contains(std::filesystem::path::value_type{}))
    {
      return makeError(Error::Code::InvalidInput, "Expected a single disk filename");
    }

#ifdef _WIN32
    if (!hasLiteralNameSyntax(component.native()))
    {
      return makeError(Error::Code::InvalidInput, "Expected a literal Windows disk filename");
    }

    return isAvailableName(component.native());
#else
    return true;
#endif
  }

  Result<std::filesystem::path> checkedFileSpelling(std::filesystem::path const& path)
  {
#ifdef _WIN32
    auto const& raw = path.native();
    auto const verbatim = raw.starts_with(kVerbatim);

    if (hasInvalidNamespaceSpelling(raw, verbatim))
    {
      return makeError(Error::Code::InvalidInput, "Invalid Windows disk namespace spelling");
    }

    if (verbatim && startsWithAsciiToken(std::wstring_view{raw}.substr(kVerbatim.size()), L"VOLUME{"))
    {
      return makeError(
        Error::Code::InvalidInput, "Windows volume GUID paths are not supported; expected a DOS drive or UNC share");
    }

    auto plain = verbatim ? ordinaryFilePath(path) : path;
    auto const native = std::wstring_view{plain.native()};
    auto const unc = native.size() >= 2 && isWindowsSeparator(native[0]) && isWindowsSeparator(native[1]);

    if (native.empty() || native.starts_with(kVerbatim) || native.contains(L'\0') ||
        (unc && !hasDiskShare(native.substr(2))) ||
        (!plain.is_absolute() && (verbatim || plain.has_root_name() || plain.has_root_directory())))
    {
      return makeError(Error::Code::InvalidInput, "Expected a Windows disk path");
    }

    // Validate the input components before absolute() can trim a relative
    // filename, and before dot segments can discard an ambiguous component.
    std::size_t depth = 0;

    for (auto const& component : plain.relative_path())
    {
      auto const& name = component.native();

      if (name.empty())
      {
        continue;
      }

      if (!verbatim && (name == L"." || name == L".."))
      {
        // filesystem::path models only the server as root_name. Never let
        // lexical normalization consume the share, even if it reappears later.
        if (unc && (depth == 0 || (name == L".." && depth == 1)))
        {
          return makeError(Error::Code::InvalidInput, "Windows disk path crosses its UNC share root");
        }

        if (name == L".." && depth != 0)
        {
          --depth;
        }

        continue;
      }

      if (!isDiskComponent(name))
      {
        return makeError(Error::Code::InvalidInput, "Ambiguous Windows disk path component");
      }

      ++depth;
    }

    plain.make_preferred();
    return plain;
#else
    return path;
#endif
  }

  Result<std::filesystem::path> fileAccessPath(std::filesystem::path const& path)
  {
#ifdef _WIN32
    auto spellingRes = checkedFileSpelling(path);

    if (!spellingRes)
    {
      return std::unexpected{spellingRes.error()};
    }

    if (!spellingRes->is_absolute())
    {
      auto ec = std::error_code{};
      auto cwd = std::filesystem::current_path(ec);

      if (ec)
      {
        return makeError(Error::Code::IoError, "Cannot determine the current directory: " + ec.message());
      }

      return fileAccessPath(cwd / *spellingRes);
    }

    // Disk I/O, unlike admission of a library path, uses the ordinary Win32
    // interpretation of dot segments before traversing reparse points.
    auto const plain = spellingRes->lexically_normal();
    auto const& native = plain.native();

    if (isDrivePath(native))
    {
      return pathFromNative(std::wstring{kVerbatim} + native);
    }

    return pathFromNative(std::wstring{kVerbatimUnc} + native.substr(2));
#else
    return path;
#endif
  }

  std::filesystem::path ordinaryFilePath(std::filesystem::path const& path)
  {
#ifdef _WIN32
    auto const native = std::wstring_view{path.native()};

    if (startsWithAsciiToken(native, kVerbatimUnc) && hasDiskShare(native.substr(kVerbatimUnc.size())))
    {
      return pathFromNative(LR"(\\)" + std::wstring{native.substr(kVerbatimUnc.size())});
    }

    if (native.starts_with(kVerbatim) && isDrivePath(native.substr(kVerbatim.size())))
    {
      return pathFromNative(native.substr(kVerbatim.size()));
    }
#endif
    return path;
  }

  Result<std::filesystem::path> weaklyCanonicalFilePath(std::filesystem::path const& path)
  {
    auto accessRes = fileAccessPath(path);

    if (!accessRes)
    {
      return std::unexpected{accessRes.error()};
    }

    auto ec = std::error_code{};
    auto resolved = std::filesystem::weakly_canonical(*accessRes, ec);

    if (ec)
    {
      return makeError(Error::Code::IoError,
                       "Failed to resolve disk path '" + pathToUtf8(ordinaryFilePath(path)) + "': " + ec.message());
    }

    return ordinaryFilePath(resolved);
  }
} // namespace ao::utility
