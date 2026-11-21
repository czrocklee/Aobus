// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ScanSpelling.h"

#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/LibraryUri.h>
#include <ao/utility/Path.h>
#include <ao/utility/String.h>

#ifdef _WIN32
#include <ao/utility/Raii.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <format>
#include <limits>
#else
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#endif

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>

namespace ao::rt
{
  namespace
  {
    using Path = std::filesystem::path;

#ifdef _WIN32
    using FileHandle = decltype(utility::makeUniquePtr<::CloseHandle>(std::declval<HANDLE>()));

    std::unexpected<Error> windowsFailure(std::string_view operation, DWORD code)
    {
      auto const unsupported =
        code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION || code == ERROR_INVALID_PARAMETER;
      return makeError(unsupported ? Error::Code::NotSupported : Error::Code::IoError,
                       std::format("Scan spelling {} failed (Win32 {}): {}",
                                   operation,
                                   code,
                                   std::system_category().message(static_cast<std::int32_t>(code))));
    }

    Result<detail::ScanLegacyFileIndex> readLegacyFileIndex(HANDLE file)
    {
      auto info = BY_HANDLE_FILE_INFORMATION{};

      if (::GetFileInformationByHandle(file, &info) == FALSE)
      {
        return windowsFailure("GetFileInformationByHandle", ::GetLastError());
      }

      if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
      {
        return makeError(Error::Code::Conflict, "Scan spelling no longer names a regular, nonredirected file");
      }

      auto sample = detail::ScanLegacyFileIndex{.volume = info.dwVolumeSerialNumber};

      if (auto const index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
          index != 0 && index != std::numeric_limits<std::uint64_t>::max())
      {
        sample.optFileIndex = index;
      }

      return sample;
    }
#endif

    struct Leaf final
    {
      std::optional<detail::ScanRegularFile> optRegular{};
#ifdef _WIN32
      // Keep the object alive until both IDs have been sampled (without denying sharing).
      FileHandle handlePtr{};
#endif
    };

    Result<Leaf> lookupLeaf(Path const& path)
    {
      auto leaf = Leaf{};
      auto identity = ScanFileIdentity{};
#ifdef _WIN32
      auto* const raw = ::CreateFileW(path.c_str(),
                                      FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);

      if (raw == INVALID_HANDLE_VALUE)
      {
        auto const code = ::GetLastError();

        if (code == ERROR_FILE_NOT_FOUND)
        {
          return Leaf{};
        }

        return windowsFailure("CreateFileW", code);
      }

      leaf.handlePtr.reset(raw);
      auto attributes = FILE_BASIC_INFO{};

      if (::GetFileInformationByHandleEx(raw, FileBasicInfo, &attributes, sizeof attributes) == FALSE)
      {
        return windowsFailure("GetFileInformationByHandleEx(FileBasicInfo)", ::GetLastError());
      }

      // Even a tag not marked as a name surrogate can be provider-mediated.
      if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
      {
        return makeError(Error::Code::Conflict, "Scan spelling lookup reached a reparse point");
      }

      ::SetLastError(ERROR_SUCCESS);
      auto const type = ::GetFileType(raw);

      if (type == FILE_TYPE_UNKNOWN)
      {
        if (auto const code = ::GetLastError(); code != ERROR_SUCCESS)
        {
          return windowsFailure("GetFileType", code);
        }
      }

      if (type != FILE_TYPE_DISK || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      {
        return makeError(Error::Code::Conflict, "Scan spelling does not name a regular file");
      }

      auto info = FILE_ID_INFO{};

      if (::GetFileInformationByHandleEx(raw, FileIdInfo, &info, sizeof info) == FALSE)
      {
        auto const code = ::GetLastError();
        auto fileRes = detail::recoverWindowsScanFileId(code, [raw] { return readLegacyFileIndex(raw); });

        if (!fileRes)
        {
          return std::unexpected{fileRes.error()};
        }

        leaf.optRegular = *fileRes;
        return leaf;
      }

      identity.volume = info.VolumeSerialNumber;
      std::ranges::copy(info.FileId.Identifier, identity.fileId.begin());

      // MS-FSCC reserves the all-ones unique ID when no ID is available.
      if (std::ranges::all_of(
            identity.fileId, [](std::uint8_t byte) { return byte == std::numeric_limits<std::uint8_t>::max(); }))
      {
        leaf.optRegular = detail::ScanFileIdUnavailable{.optVolume = identity.volume};
        return leaf;
      }
#else
      using FileStatus = struct ::stat;
      auto info = FileStatus{};

      if (::lstat(path.c_str(), &info) != 0)
      {
        if (errno == ENOENT)
        {
          return Leaf{};
        }

        return makeError(Error::Code::IoError, std::generic_category().message(errno));
      }

      if (S_ISLNK(info.st_mode))
      {
        return makeError(Error::Code::Conflict, "Scan spelling lookup reached a symbolic link");
      }

      if (!S_ISREG(info.st_mode))
      {
        return makeError(Error::Code::Conflict, "Scan spelling does not name a regular file");
      }

      identity.volume = static_cast<std::uint64_t>(info.st_dev);
      static_assert(sizeof info.st_ino <= sizeof identity.fileId);
      std::memcpy(identity.fileId.data(), &info.st_ino, sizeof info.st_ino);
#endif
      if (std::ranges::all_of(identity.fileId, [](std::uint8_t byte) { return byte == 0; }))
      {
        leaf.optRegular = detail::ScanFileIdUnavailable{.optVolume = identity.volume};
        return leaf;
      }

      leaf.optRegular = identity;
      return leaf;
    }

    Result<detail::ScanSpellingLookup> lookupPair(Path const& oldPath, Path const& newPath)
    {
      auto const oldRes = lookupLeaf(oldPath);

      if (!oldRes)
      {
        return std::unexpected{oldRes.error()};
      }

      if (!oldRes->optRegular)
      {
        // Absent old lookup is nonalias evidence, provided the destination
        // still exists. No ID is required on a case-sensitive filesystem.
        if (auto ec = std::error_code{}; !std::filesystem::exists(std::filesystem::symlink_status(newPath, ec)) || ec)
        {
          return makeError(Error::Code::Conflict, "Cannot confirm scan spelling destination is present");
        }

        return DistinctScanSpellings{};
      }

      auto const newRes = lookupLeaf(newPath);

      if (!newRes)
      {
        return std::unexpected{newRes.error()};
      }

      if (!newRes->optRegular)
      {
        return makeError(Error::Code::Conflict, "Scan spelling destination disappeared");
      }

      return detail::ScanRegularFilePair{.oldFile = *oldRes->optRegular, .newFile = *newRes->optRegular};
    }

    Path::string_type foldedName(Path::string_type name)
    {
      // Fold native code units: unrelated entries need not be valid Unicode.
      for (auto& unit : name)
      {
        if (unit >= 'A' && unit <= 'Z')
        {
          unit = static_cast<Path::value_type>(unit + ('a' - 'A'));
        }
      }

      return name;
    }

    Result<Path> resolveParent(Path const& root, std::string_view parentUri)
    {
      auto ec = std::error_code{};
      auto resolvedRoot = std::filesystem::canonical(root, ec);

      if (ec)
      {
        return makeError(Error::Code::IoError, ec.message());
      }

      if (parentUri.empty())
      {
        return resolvedRoot;
      }

      auto const parsedRes = library::LibraryUri::parse(parentUri);

      if (!parsedRes)
      {
        return std::unexpected{parsedRes.error()};
      }

#ifdef _WIN32
      // Not all reparse points change the canonical path. Reject provider-
      // mediated parents too, checking each component below the resolved root.
      auto current = resolvedRoot;

      for (auto const& component : utility::pathFromUtf8(parentUri))
      {
        current /= component;
        auto const attributes = ::GetFileAttributesW(current.c_str());

        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
          return windowsFailure("GetFileAttributesW(parent)", ::GetLastError());
        }

        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
          return makeError(Error::Code::Conflict, "Scan spelling parent is a reparse point");
        }
      }
#endif
      auto directoryRes = parsedRes->resolveUnder(resolvedRoot);

      if (directoryRes && utility::pathToGenericUtf8(directoryRes->lexically_relative(resolvedRoot)) != parentUri)
      {
        return makeError(Error::Code::Conflict, "Scan spelling parent is redirected");
      }

      return directoryRes;
    }
  } // namespace

#ifdef _WIN32
  namespace detail
  {
    Result<ScanRegularFile> recoverWindowsScanFileId(std::uint32_t code,
                                                     std::function<Result<ScanLegacyFileIndex>()> const& readLegacy)
    {
      if (code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION)
      {
        return ScanFileIdUnavailable{};
      }

      if (code != ERROR_INVALID_PARAMETER)
      {
        return windowsFailure("GetFileInformationByHandleEx(FileIdInfo)", code);
      }

      auto const legacyRes = readLegacy();

      if (!legacyRes)
      {
        return std::unexpected{legacyRes.error()};
      }

      return *legacyRes;
    }
  } // namespace detail
#endif

  std::string scanSpellingKey(std::string_view uri)
  {
    auto const leaf = uri.rfind('/') + 1;
    return std::string{uri.substr(0, leaf)} + utility::toLower(uri.substr(leaf));
  }

  ScanSpellingInspection::ScanSpellingInspection(Path root, std::stop_token stopToken)
    : _root{std::move(root)}, _stopToken{std::move(stopToken)}
  {
  }

  Result<ScanSpellingObservation> ScanSpellingInspection::inspect(std::string_view oldUri, std::string_view newUri)
  {
    async::throwIfStopRequested(_stopToken);

    try
    {
      auto const nameStart = newUri.rfind('/') + 1;

      if (auto const parsedRes = library::LibraryUri::parse(newUri); !parsedRes || parsedRes->value() != newUri ||
                                                                     oldUri == newUri ||
                                                                     scanSpellingKey(oldUri) != scanSpellingKey(newUri))
      {
        return makeError(
          Error::Code::InvalidInput, "Scan spelling requires canonical URIs differing only in basename case");
      }

#ifdef _WIN32
      // Win32 normalization can strip dots/spaces or treat a colon as a stream or drive.
      for (auto const& component : utility::pathFromUtf8(newUri))
      {
        if (auto const& value = component.native();
            value.ends_with(L'.') || value.ends_with(L' ') || value.contains(L':'))
        {
          return makeError(Error::Code::InvalidInput, "Ambiguous Windows scan spelling path component");
        }
      }
#endif
      auto const directoryRes = resolveParent(_root, newUri.substr(0, nameStart == 0 ? 0 : nameStart - 1));

      if (!directoryRes)
      {
        return std::unexpected{directoryRes.error()};
      }

      auto const oldName = utility::pathFromUtf8(oldUri.substr(nameStart));
      auto const newName = utility::pathFromUtf8(newUri.substr(nameStart));
      auto entry = _directories.find(*directoryRes);

      if (entry == _directories.end())
      {
        entry = _directories.emplace(*directoryRes, readStableNames(*directoryRes)).first;
      }

      auto const& indexRes = entry->second;

      if (!indexRes)
      {
        return std::unexpected{indexRes.error()};
      }

      return indexRes->observe(oldName.native(),
                               newName.native(),
                               [&] { return lookupPair(*directoryRes / oldName, *directoryRes / newName); });
    }
    catch (std::filesystem::filesystem_error const& error)
    {
      return makeError(Error::Code::IoError, error.what());
    }
  }

  Result<detail::ScanSpellingDirectory> ScanSpellingInspection::readNames(Path const& directory) const
  {
    async::throwIfStopRequested(_stopToken);
    auto names = detail::ScanSpellingDirectory{};
    auto ec = std::error_code{};

    for (auto iter = std::filesystem::directory_iterator{directory, ec};
         !ec && iter != std::filesystem::directory_iterator{};
         iter.increment(ec))
    {
      async::throwIfStopRequested(_stopToken);
      auto const name = iter->path().filename();
      names.add(name.native());
    }

    if (ec)
    {
      return makeError(Error::Code::IoError, ec.message());
    }

    return names;
  }

  Result<detail::ScanSpellingDirectory> ScanSpellingInspection::readStableNames(Path const& directory) const
  {
    auto firstRes = readNames(directory);

    if (!firstRes)
    {
      return firstRes;
    }

    auto const secondRes = readNames(directory);

    if (!secondRes)
    {
      return std::unexpected{secondRes.error()};
    }

    firstRes->retainStableGroups(*secondRes, _stopToken);
    return firstRes;
  }

  void detail::ScanSpellingDirectory::add(NativeName const& name)
  {
    _groups[foldedName(name)].insert(name);
  }

  void detail::ScanSpellingDirectory::retainStableGroups(ScanSpellingDirectory const& second, std::stop_token stopToken)
  {
    // A torn group cannot certify any spelling; unrelated groups remain usable.
    std::erase_if(_groups,
                  [&](auto const& group)
                  {
                    async::throwIfStopRequested(stopToken);
                    auto const other = second._groups.find(group.first);
                    return other == second._groups.end() || group.second != other->second;
                  });
  }

  Result<ScanSpellingObservation> detail::ScanSpellingDirectory::observe(
    NativeName const& oldName,
    NativeName const& newName,
    std::function<Result<ScanSpellingLookup>()> const& lookup) const
  {
    auto const group = _groups.find(foldedName(newName));

    if (group == _groups.end() || !group->second.contains(newName))
    {
      return makeError(Error::Code::Conflict, "Expected scan spelling directory entry is missing or unstable");
    }

    // Two exact names are distinct entries, even hardlinks or pointers.
    if (group->second.contains(oldName))
    {
      return DistinctScanSpellings{};
    }

    auto const lookupRes = lookup();

    if (!lookupRes)
    {
      return std::unexpected{lookupRes.error()};
    }

    if (std::holds_alternative<DistinctScanSpellings>(*lookupRes))
    {
      return DistinctScanSpellings{};
    }

    auto const& [oldFile, newFile] = std::get<ScanRegularFilePair>(*lookupRes);

    if (group->second.size() != 1 || oldFile.index() != newFile.index())
    {
      return makeError(Error::Code::Conflict, "Ambiguous scan spelling directory entries or file identity");
    }

    if (auto const* identity = std::get_if<ScanFileIdentity>(&oldFile); identity != nullptr)
    {
      if (*identity != std::get<ScanFileIdentity>(newFile))
      {
        return makeError(Error::Code::Conflict, "Conflicting scan spelling file identities");
      }

      return *identity;
    }

    if (auto const* legacy = std::get_if<ScanLegacyFileIndex>(&oldFile); legacy != nullptr)
    {
      if (*legacy != std::get<ScanLegacyFileIndex>(newFile))
      {
        return makeError(Error::Code::Conflict, "Conflicting legacy scan spelling file information");
      }

      return ScanSpellingNeedsFingerprint{};
    }

    auto const& optOldVolume = std::get<ScanFileIdUnavailable>(oldFile).optVolume;
    auto const& optNewVolume = std::get<ScanFileIdUnavailable>(newFile).optVolume;

    if (optOldVolume && optNewVolume && optOldVolume != optNewVolume)
    {
      return makeError(Error::Code::Conflict, "Conflicting scan spelling volumes");
    }

    return ScanSpellingNeedsFingerprint{};
  }
} // namespace ao::rt
