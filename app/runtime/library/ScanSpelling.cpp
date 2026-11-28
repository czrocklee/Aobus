// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ScanSpelling.h"

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/LibraryUri.h>
#include <ao/utility/FileAccessPath.h>
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
#include <cstddef>
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

    enum class ScanEntryKind : std::uint8_t
    {
      Absent,
      RegularFile,
      Directory,
      Other,
    };

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

    Result<detail::ScanLegacyFileIndex> readLegacyFileIndex(HANDLE handle, ScanEntryKind kind)
    {
      auto info = BY_HANDLE_FILE_INFORMATION{};

      if (::GetFileInformationByHandle(handle, &info) == FALSE)
      {
        return windowsFailure("GetFileInformationByHandle", ::GetLastError());
      }

      auto const isDirectory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

      if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
          isDirectory != (kind == ScanEntryKind::Directory))
      {
        return makeError(Error::Code::Conflict,
                         kind == ScanEntryKind::Directory
                           ? "Scan spelling no longer names a nonredirected directory"
                           : "Scan spelling no longer names a regular, nonredirected file");
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

    struct DirectEntry final
    {
      ScanEntryKind kind = ScanEntryKind::Absent;
#ifdef _WIN32
      // Hold the probed objects through comparison without denying sharing.
      FileHandle handlePtr{};
#else
      using FileStatus = struct ::stat;
      std::optional<FileStatus> optStatus{};
#endif
      bool isPresent() const noexcept { return kind != ScanEntryKind::Absent; }
    };

    Result<DirectEntry> probeDirectEntry(Path const& path)
    {
#ifdef _WIN32
      auto accessRes = utility::fileAccessPath(path);

      if (!accessRes)
      {
        return std::unexpected{accessRes.error()};
      }

      auto* const raw = ::CreateFileW(accessRes->c_str(),
                                      FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);

      if (raw == INVALID_HANDLE_VALUE)
      {
        auto const code = ::GetLastError();

        // ERROR_PATH_NOT_FOUND can also be returned for a live Win32 path.
        if (code == ERROR_FILE_NOT_FOUND)
        {
          return DirectEntry{};
        }

        return windowsFailure("CreateFileW", code);
      }

      auto entry = DirectEntry{};
      entry.handlePtr.reset(raw);
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

      entry.kind = ScanEntryKind::Other;

      if (type == FILE_TYPE_DISK)
      {
        auto const isDirectory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.kind = isDirectory ? ScanEntryKind::Directory : ScanEntryKind::RegularFile;
      }

      return entry;
#else
      auto info = DirectEntry::FileStatus{};

      if (::lstat(path.c_str(), &info) != 0)
      {
        if (errno == ENOENT)
        {
          return DirectEntry{};
        }

        return makeError(Error::Code::IoError, std::generic_category().message(errno));
      }

      if (S_ISLNK(info.st_mode))
      {
        return makeError(Error::Code::Conflict, "Scan spelling lookup reached a symbolic link");
      }

      auto kind = ScanEntryKind::Other;

      if (S_ISDIR(info.st_mode))
      {
        kind = ScanEntryKind::Directory;
      }
      else if (S_ISREG(info.st_mode))
      {
        kind = ScanEntryKind::RegularFile;
      }

      return DirectEntry{.kind = kind, .optStatus = info};
#endif
    }

    std::unexpected<Error> unexpectedEntryKind(ScanEntryKind kind)
    {
      return makeError(Error::Code::Conflict,
                       kind == ScanEntryKind::Directory ? "Scan spelling does not name a directory"
                                                        : "Scan spelling does not name a regular file");
    }

    Result<DirectEntry> lookupDirectEntry(Path const& path, ScanEntryKind kind)
    {
      auto entryRes = probeDirectEntry(path);

      if (entryRes && entryRes->isPresent() && entryRes->kind != kind)
      {
        return unexpectedEntryKind(kind);
      }

      return entryRes;
    }

    Result<detail::ScanNativeIdentity> readEntryIdentity(DirectEntry const& entry, [[maybe_unused]] ScanEntryKind kind)
    {
      auto identity = ScanFileIdentity{};
#ifdef _WIN32
      AO_INVARIANT(entry.handlePtr);
      auto* const raw = entry.handlePtr.get();
      auto info = FILE_ID_INFO{};

      if (::GetFileInformationByHandleEx(raw, FileIdInfo, &info, sizeof info) == FALSE)
      {
        auto const code = ::GetLastError();
        return detail::recoverWindowsScanFileId(code, [raw, kind] { return readLegacyFileIndex(raw, kind); });
      }

      identity.volume = info.VolumeSerialNumber;
      std::ranges::copy(info.FileId.Identifier, identity.fileId.begin());

      // MS-FSCC reserves the all-ones unique ID when no ID is available.
      if (std::ranges::all_of(
            identity.fileId, [](std::uint8_t byte) { return byte == std::numeric_limits<std::uint8_t>::max(); }))
      {
        return detail::ScanFileIdUnavailable{.optVolume = identity.volume};
      }
#else
      AO_INVARIANT(entry.optStatus);
      auto const& info = *entry.optStatus;
      identity.volume = static_cast<std::uint64_t>(info.st_dev);
      static_assert(sizeof info.st_ino <= sizeof identity.fileId);
      std::memcpy(identity.fileId.data(), &info.st_ino, sizeof info.st_ino);
#endif
      if (std::ranges::all_of(identity.fileId, [](std::uint8_t byte) { return byte == 0; }))
      {
        return detail::ScanFileIdUnavailable{.optVolume = identity.volume};
      }

      return identity;
    }

    struct Leaf final
    {
      std::optional<detail::ScanNativeIdentity> optRegular{};
      DirectEntry entry{};
    };

    Result<Leaf> lookupLeaf(Path const& path)
    {
      auto entryRes = lookupDirectEntry(path, ScanEntryKind::RegularFile);

      if (!entryRes)
      {
        return std::unexpected{entryRes.error()};
      }

      if (!entryRes->isPresent())
      {
        return Leaf{};
      }

      auto const identityRes = readEntryIdentity(*entryRes, ScanEntryKind::RegularFile);

      if (!identityRes)
      {
        return std::unexpected{identityRes.error()};
      }

      return Leaf{.optRegular = *identityRes, .entry = std::move(*entryRes)};
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
        auto accessRes = utility::fileAccessPath(newPath);

        if (!accessRes)
        {
          return std::unexpected{accessRes.error()};
        }

        if (auto ec = std::error_code{};
            !std::filesystem::exists(std::filesystem::symlink_status(*accessRes, ec)) || ec)
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

    Result<DirectEntry> lookupDirectPath(Path current,
                                         Path const& relative,
                                         ScanEntryKind finalKind,
                                         std::stop_token stopToken)
    {
      for (auto const& component : relative.parent_path())
      {
        async::throwIfStopRequested(stopToken);
        current /= component;
        auto entryRes = lookupDirectEntry(current, ScanEntryKind::Directory);

        if (!entryRes || !entryRes->isPresent())
        {
          return entryRes;
        }
      }

      return lookupDirectEntry(relative.empty() ? current : current / relative.filename(), finalKind);
    }

    bool hasExactEntryName(Path const& path, std::stop_token stopToken)
    {
      async::throwIfStopRequested(stopToken);
      auto ec = std::error_code{};
      auto const name = path.filename();

      auto parentRes = utility::fileAccessPath(path.parent_path());

      if (!parentRes)
      {
        return false;
      }

      for (auto iter = std::filesystem::directory_iterator{*parentRes, ec};
           !ec && iter != std::filesystem::directory_iterator{};
           iter.increment(ec))
      {
        async::throwIfStopRequested(stopToken);

        if (iter->path().filename().native() == name.native())
        {
          return true;
        }
      }

      return false;
    }

    bool hasExactRegularAncestor(Path const& root, Path const& prefix, Path const& suffix, std::stop_token stopToken)
    {
      async::throwIfStopRequested(stopToken);

      if (auto const rootRes = lookupDirectEntry(root, ScanEntryKind::Directory); !rootRes || !rootRes->isPresent())
      {
        return false;
      }

      if (!prefix.empty())
      {
        if (auto const prefixRes = lookupDirectPath(root, prefix, ScanEntryKind::Directory, stopToken);
            !prefixRes || !prefixRes->isPresent())
        {
          return false;
        }
      }

      auto ancestor = prefix;

      for (auto const& component : suffix.parent_path())
      {
        async::throwIfStopRequested(stopToken);
        ancestor /= component;
        auto const path = root / ancestor;
        auto const entryRes = probeDirectEntry(path);

        if (!entryRes || !entryRes->isPresent())
        {
          return false;
        }

        if (entryRes->kind != ScanEntryKind::Directory)
        {
          if (entryRes->kind != ScanEntryKind::RegularFile || !hasExactEntryName(path, stopToken))
          {
            return false;
          }

          // Recheck the raw route after the listing. Never interpret a generic
          // native path error or an intervening redirect as a missing descendant.
          async::throwIfStopRequested(stopToken);
          auto const confirmedRes = lookupDirectPath(root, ancestor, ScanEntryKind::RegularFile, stopToken);
          return confirmedRes && confirmedRes->isPresent();
        }
      }

      return false;
    }

    Result<detail::ScanSpellingDirectory> readScanNames(Path const& directory, std::stop_token stopToken)
    {
      async::throwIfStopRequested(stopToken);
      auto names = detail::ScanSpellingDirectory{};
      auto ec = std::error_code{};

      auto accessRes = utility::fileAccessPath(directory);

      if (!accessRes)
      {
        return std::unexpected{accessRes.error()};
      }

      for (auto iter = std::filesystem::directory_iterator{*accessRes, ec};
           !ec && iter != std::filesystem::directory_iterator{};
           iter.increment(ec))
      {
        async::throwIfStopRequested(stopToken);
        auto const name = iter->path().filename();
        names.add(name.native());
      }

      if (ec)
      {
        return makeError(Error::Code::IoError, ec.message());
      }

      return names;
    }

    Result<> validateIndependentPeerSuffixes(Path const& root,
                                             Path const& oldPath,
                                             Path const& newPath,
                                             Path const& suffix,
                                             ScanEntryKind oldKind,
                                             std::stop_token stopToken)
    {
      auto const newFileRes = lookupDirectPath(newPath, suffix, ScanEntryKind::RegularFile, stopToken);

      if (!newFileRes)
      {
        return std::unexpected{newFileRes.error()};
      }

      if (!newFileRes->isPresent())
      {
        return makeError(Error::Code::Conflict, "Scan directory spelling destination disappeared");
      }

      if (oldKind == ScanEntryKind::Directory)
      {
        // Absence is safe, as is a positively identified regular file in
        // place of a suffix directory. Recheck from the root to reject redirects.
        if (auto const oldFileRes = lookupDirectPath(oldPath, suffix, ScanEntryKind::RegularFile, stopToken);
            !oldFileRes && !hasExactRegularAncestor(root, oldPath.lexically_relative(root), suffix, stopToken))
        {
          return std::unexpected{oldFileRes.error()};
        }
      }

      return {};
    }

    Result<> validateExactRegularSibling(Path const& root,
                                         Path const& parent,
                                         Path const& oldName,
                                         Path const& newName,
                                         std::stop_token stopToken)
    {
      // Corroborate the exact names, then recheck the raw route and both kinds.
      auto const freshRes = readScanNames(parent, stopToken);

      if (!freshRes)
      {
        return std::unexpected{freshRes.error()};
      }

      if (auto const namesRes = freshRes->compareNames(oldName.native(), newName.native());
          !namesRes || *namesRes != detail::ScanSpellingDirectory::NameMatch::BothExact)
      {
        return makeError(Error::Code::Conflict, "Scan directory spelling entries changed during type lookup");
      }

      async::throwIfStopRequested(stopToken);
      // Probe the root itself, not root/., which would follow a redirect.
      auto const common = parent == root ? Path{} : parent.lexically_relative(root);
      auto const commonRes = lookupDirectPath(root, common, ScanEntryKind::Directory, stopToken);

      if (!commonRes)
      {
        return std::unexpected{commonRes.error()};
      }

      if (!commonRes->isPresent())
      {
        return makeError(Error::Code::Conflict, "Shared scan spelling parent disappeared");
      }

      for (auto const& [name, kind] :
           {std::pair{&oldName, ScanEntryKind::RegularFile}, std::pair{&newName, ScanEntryKind::Directory}})
      {
        async::throwIfStopRequested(stopToken);
        auto const entryRes = lookupDirectEntry(parent / *name, kind);

        if (!entryRes)
        {
          return std::unexpected{entryRes.error()};
        }

        if (!entryRes->isPresent())
        {
          return makeError(Error::Code::Conflict, "Scan directory spelling entries disappeared during type lookup");
        }
      }

      return {};
    }

    struct DirectDirectoryPair final
    {
      DirectEntry oldDirectory;
      DirectEntry newDirectory;
    };
    using DirectorySpellingLookup = std::variant<DistinctScanSpellings, DirectDirectoryPair>;

    Result<DirectorySpellingLookup> lookupDirectorySpellings(Path const& root,
                                                             Path const& parent,
                                                             Path const& oldName,
                                                             Path const& newName,
                                                             detail::ScanSpellingDirectory::NameMatch names,
                                                             Path const& suffix,
                                                             ScanDirectoryProbe probe,
                                                             std::stop_token stopToken)
    {
      using NameMatch = detail::ScanSpellingDirectory::NameMatch;
      auto const oldPath = parent / oldName;
      auto const newPath = parent / newName;
      auto oldDirectoryRes = probeDirectEntry(oldPath);

      if (!oldDirectoryRes)
      {
        return std::unexpected{oldDirectoryRes.error()};
      }

      auto const oldIsExactRegular =
        names == NameMatch::BothExact && oldDirectoryRes->kind == ScanEntryKind::RegularFile;

      if (oldDirectoryRes->isPresent() && oldDirectoryRes->kind != ScanEntryKind::Directory && !oldIsExactRegular)
      {
        return unexpectedEntryKind(ScanEntryKind::Directory);
      }

      auto newDirectoryRes = lookupDirectEntry(newPath, ScanEntryKind::Directory);

      if (!newDirectoryRes)
      {
        return std::unexpected{newDirectoryRes.error()};
      }

      if (!newDirectoryRes->isPresent() || (names == NameMatch::BothExact && !oldDirectoryRes->isPresent()))
      {
        return makeError(Error::Code::Conflict, "Listed scan spelling directory disappeared");
      }

      if (oldIsExactRegular)
      {
        // A distinct, exact regular file cannot resolve the old descendant.
        if (auto const freshRes = validateExactRegularSibling(root, parent, oldName, newName, stopToken); !freshRes)
        {
          return std::unexpected{freshRes.error()};
        }
      }

      if (names != NameMatch::BothExact && oldDirectoryRes->isPresent())
      {
        return DirectDirectoryPair{
          .oldDirectory = std::move(*oldDirectoryRes), .newDirectory = std::move(*newDirectoryRes)};
      }

      // Structural distinction is enough for planning, but final peer
      // admission must also rule out redirects below those directory entries.
      if (probe == ScanDirectoryProbe::IndependentPeer)
      {
        if (auto const suffixRes =
              validateIndependentPeerSuffixes(root, oldPath, newPath, suffix, oldDirectoryRes->kind, stopToken);
            !suffixRes)
        {
          return std::unexpected{suffixRes.error()};
        }
      }

      return DistinctScanSpellings{};
    }

    struct EntryIdentityPair final
    {
      detail::ScanNativeIdentity oldIdentity;
      detail::ScanNativeIdentity newIdentity;
    };

    Result<EntryIdentityPair> readIdentityPair(DirectEntry const& oldEntry,
                                               DirectEntry const& newEntry,
                                               ScanEntryKind kind)
    {
      auto const oldIdentityRes = readEntryIdentity(oldEntry, kind);

      if (!oldIdentityRes)
      {
        return std::unexpected{oldIdentityRes.error()};
      }

      auto const newIdentityRes = readEntryIdentity(newEntry, kind);

      if (!newIdentityRes)
      {
        return std::unexpected{newIdentityRes.error()};
      }

      return EntryIdentityPair{.oldIdentity = *oldIdentityRes, .newIdentity = *newIdentityRes};
    }

    Result<std::optional<ScanFileIdentity>> reconcileEntryIdentities(detail::ScanNativeIdentity const& oldIdentity,
                                                                     detail::ScanNativeIdentity const& newIdentity)
    {
      if (oldIdentity.index() != newIdentity.index())
      {
        return makeError(Error::Code::Conflict, "Ambiguous scan spelling directory entries or file identity");
      }

      if (auto const* identity = std::get_if<ScanFileIdentity>(&oldIdentity); identity != nullptr)
      {
        if (*identity != std::get<ScanFileIdentity>(newIdentity))
        {
          return makeError(Error::Code::Conflict, "Conflicting scan spelling file identities");
        }

        return *identity;
      }

      if (auto const* legacy = std::get_if<detail::ScanLegacyFileIndex>(&oldIdentity); legacy != nullptr)
      {
        if (*legacy != std::get<detail::ScanLegacyFileIndex>(newIdentity))
        {
          return makeError(Error::Code::Conflict, "Conflicting legacy scan spelling file information");
        }

        return std::nullopt;
      }

      auto const& optOldVolume = std::get<detail::ScanFileIdUnavailable>(oldIdentity).optVolume;
      auto const& optNewVolume = std::get<detail::ScanFileIdUnavailable>(newIdentity).optVolume;

      if (optOldVolume && optNewVolume && optOldVolume != optNewVolume)
      {
        return makeError(Error::Code::Conflict, "Conflicting scan spelling volumes");
      }

      return std::nullopt;
    }

    std::optional<std::size_t> differingDirectoryComponent(std::string_view oldUri, std::string_view newUri) noexcept
    {
      auto const leaf = newUri.rfind('/') + 1;

      if (leaf == 0 || oldUri.size() != newUri.size() || oldUri.substr(leaf) != newUri.substr(leaf))
      {
        return {};
      }

      std::size_t componentStart = 0;
      auto optDifference = std::optional<std::size_t>{};

      for (std::size_t i = 0; i < leaf; ++i)
      {
        if (oldUri[i] != newUri[i])
        {
          if ((optDifference && *optDifference != componentStart) ||
              utility::toAsciiLower(oldUri[i]) != utility::toAsciiLower(newUri[i]))
          {
            return {};
          }

          optDifference = componentStart;
        }

        if (newUri[i] == '/')
        {
          componentStart = i + 1;
        }
      }

      return optDifference;
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

    Result<Path> canonicalScanRoot(Path const& root)
    {
      auto accessRes = utility::fileAccessPath(root);

      if (!accessRes)
      {
        return std::unexpected{accessRes.error()};
      }

      auto ec = std::error_code{};
      auto resolvedRoot = std::filesystem::canonical(*accessRes, ec);

      if (ec)
      {
        return makeError(Error::Code::IoError, ec.message());
      }

      return utility::ordinaryFilePath(resolvedRoot);
    }

    Result<Path> resolveParent(Path const& root, std::string_view parentUri)
    {
      auto rootRes = canonicalScanRoot(root);

      if (!rootRes || parentUri.empty())
      {
        return rootRes;
      }

      auto const& resolvedRoot = *rootRes;

      auto const parsedRes = library::LibraryUri::parse(parentUri);

      if (!parsedRes)
      {
        return std::unexpected{parsedRes.error()};
      }

#ifdef _WIN32
      // Not all reparse points change the canonical path. Reject provider-
      // mediated parents too, checking each component below the resolved root.
      auto currentRes = utility::fileAccessPath(resolvedRoot);

      if (!currentRes)
      {
        return std::unexpected{currentRes.error()};
      }

      auto& current = *currentRes;

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
    Result<ScanNativeIdentity> recoverWindowsScanFileId(std::uint32_t code,
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

  Result<ScanSpellingObservation> detail::reconcileScanDirectorySpelling(ScanNativeIdentity const& oldDirectory,
                                                                         ScanNativeIdentity const& newDirectory,
                                                                         ScanNativeIdentity const& oldFile,
                                                                         ScanNativeIdentity const& newFile)
  {
    auto const directoryRes = reconcileEntryIdentities(oldDirectory, newDirectory);

    if (!directoryRes)
    {
      return std::unexpected{directoryRes.error()};
    }

    auto const fileRes = reconcileEntryIdentities(oldFile, newFile);

    if (!fileRes)
    {
      return std::unexpected{fileRes.error()};
    }

    auto const& optDirectory = *directoryRes;
    auto const& optFile = *fileRes;

    if (optDirectory && optFile)
    {
      return ScanDirectorySpellingIdentity{.directory = *optDirectory, .file = *optFile};
    }

    return ScanDirectorySpellingNeedsFingerprint{.optDirectory = optDirectory, .optFile = optFile};
  }

  bool matchesScanDirectorySpellingFallback(ScanDirectorySpellingNeedsFingerprint const& planned,
                                            ScanSpellingObservation const& live) noexcept
  {
    if (auto const* identity = std::get_if<ScanDirectorySpellingIdentity>(&live); identity != nullptr)
    {
      return (!planned.optDirectory || planned.optDirectory == identity->directory) &&
             (!planned.optFile || planned.optFile == identity->file);
    }

    if (auto const* partial = std::get_if<ScanDirectorySpellingNeedsFingerprint>(&live); partial != nullptr)
    {
      return (!planned.optDirectory || planned.optDirectory == partial->optDirectory) &&
             (!planned.optFile || planned.optFile == partial->optFile);
    }

    return false;
  }

  std::string scanSpellingKey(std::string_view uri)
  {
    auto const leaf = uri.rfind('/') + 1;
    return std::string{uri.substr(0, leaf)} + utility::toLower(uri.substr(leaf));
  }

  std::string scanDirectorySpellingKey(std::string_view uri)
  {
    auto const leaf = uri.rfind('/') + 1;
    return utility::toLower(uri.substr(0, leaf)).append(uri.substr(leaf));
  }

  bool isScanDirectorySpellingPair(std::string_view oldUri, std::string_view newUri) noexcept
  {
    return differingDirectoryComponent(oldUri, newUri).has_value();
  }

  bool hasExactRegularScanAncestor(Path const& root, std::string_view uri, std::stop_token stopToken)
  {
    async::throwIfStopRequested(stopToken);

    if (auto const parsedRes = library::LibraryUri::parse(uri); !parsedRes || parsedRes->value() != uri)
    {
      return false;
    }

    auto const relative = utility::pathFromUtf8(uri);

#ifdef _WIN32
    if (!utility::checkedFileSpelling(relative))
    {
      return false;
    }
#endif
    auto const rootRes = canonicalScanRoot(root);
    return rootRes && hasExactRegularAncestor(*rootRes, {}, relative, stopToken);
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
      if (auto spellingRes = utility::checkedFileSpelling(utility::pathFromUtf8(newUri)); !spellingRes)
      {
        return std::unexpected{spellingRes.error()};
      }
#endif
      auto const directoryRes = resolveParent(_root, newUri.substr(0, nameStart == 0 ? 0 : nameStart - 1));

      if (!directoryRes)
      {
        return std::unexpected{directoryRes.error()};
      }

      auto const oldName = utility::pathFromUtf8(oldUri.substr(nameStart));
      auto const newName = utility::pathFromUtf8(newUri.substr(nameStart));
      auto const& indexRes = stableNames(*directoryRes);

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

  Result<ScanSpellingObservation> ScanSpellingInspection::inspectDirectory(std::string_view oldUri,
                                                                           std::string_view newUri,
                                                                           ScanDirectoryProbe probe)
  {
    async::throwIfStopRequested(_stopToken);

    try
    {
      using NameMatch = detail::ScanSpellingDirectory::NameMatch;
      auto const optDifference = differingDirectoryComponent(oldUri, newUri);
      auto const oldUriRes = library::LibraryUri::parse(oldUri);

      if (auto const newUriRes = library::LibraryUri::parse(newUri);
          !optDifference || !oldUriRes || oldUriRes->value() != oldUri || !newUriRes || newUriRes->value() != newUri)
      {
        return makeError(
          Error::Code::InvalidInput,
          "Scan directory spelling requires canonical URIs differing only in one parent component's case");
      }

#ifdef _WIN32
      if (auto spellingRes = utility::checkedFileSpelling(utility::pathFromUtf8(newUri)); !spellingRes)
      {
        return std::unexpected{spellingRes.error()};
      }
#endif
      auto const rootRes = canonicalScanRoot(_root);

      if (!rootRes)
      {
        return std::unexpected{rootRes.error()};
      }

      auto const& root = *rootRes;

      auto const start = *optDifference;
      auto const end = newUri.find('/', start);
      auto const common = utility::pathFromUtf8(newUri.substr(0, start == 0 ? 0 : start - 1));
      auto const oldName = utility::pathFromUtf8(oldUri.substr(start, end - start));
      auto const newName = utility::pathFromUtf8(newUri.substr(start, end - start));
      auto const suffix = utility::pathFromUtf8(newUri.substr(end + 1));

      // Check raw prefixes before using them. Canonicalizing either arm would hide redirects.
      auto const commonRes = lookupDirectPath(root, common, ScanEntryKind::Directory, _stopToken);

      if (!commonRes)
      {
        return std::unexpected{commonRes.error()};
      }

      if (!commonRes->isPresent())
      {
        return makeError(Error::Code::Conflict, "Shared scan spelling parent disappeared");
      }

      auto const parent = common.empty() ? root : root / common;
      auto const& indexRes = stableNames(parent);

      if (!indexRes)
      {
        return std::unexpected{indexRes.error()};
      }

      auto const namesRes = indexRes->compareNames(oldName.native(), newName.native());

      if (!namesRes)
      {
        return std::unexpected{namesRes.error()};
      }

      auto const directoriesRes =
        lookupDirectorySpellings(root, parent, oldName, newName, *namesRes, suffix, probe, _stopToken);

      if (!directoriesRes)
      {
        return std::unexpected{directoriesRes.error()};
      }

      if (std::holds_alternative<DistinctScanSpellings>(*directoriesRes))
      {
        return DistinctScanSpellings{};
      }

      auto const newFileRes = lookupDirectPath(parent / newName, suffix, ScanEntryKind::RegularFile, _stopToken);

      if (!newFileRes)
      {
        return std::unexpected{newFileRes.error()};
      }

      if (!newFileRes->isPresent())
      {
        return makeError(Error::Code::Conflict, "Scan directory spelling destination disappeared");
      }

      if (*namesRes != NameMatch::Singleton)
      {
        return makeError(Error::Code::Conflict, "Ambiguous scan directory spelling entries");
      }

      auto const& [oldDirectory, newDirectory] = std::get<DirectDirectoryPair>(*directoriesRes);
      auto const directoryRes = readIdentityPair(oldDirectory, newDirectory, ScanEntryKind::Directory);

      if (!directoryRes)
      {
        return std::unexpected{directoryRes.error()};
      }

      auto const oldFileRes = lookupDirectPath(parent / oldName, suffix, ScanEntryKind::RegularFile, _stopToken);

      if (!oldFileRes)
      {
        return std::unexpected{oldFileRes.error()};
      }

      if (!oldFileRes->isPresent())
      {
        return makeError(Error::Code::Conflict, "File disappeared through the old scan directory spelling");
      }

      auto const fileRes = readIdentityPair(*oldFileRes, *newFileRes, ScanEntryKind::RegularFile);

      if (!fileRes)
      {
        return std::unexpected{fileRes.error()};
      }

      auto proofRes = detail::reconcileScanDirectorySpelling(
        directoryRes->oldIdentity, directoryRes->newIdentity, fileRes->oldIdentity, fileRes->newIdentity);

      if (proofRes && std::holds_alternative<ScanDirectorySpellingNeedsFingerprint>(*proofRes))
      {
        // Without complete IDs, corroborate the cached singleton after checking both raw paths.
        if (auto const freshRes = corroborateDirectorySingleton(parent, oldName, newName); !freshRes)
        {
          return std::unexpected{freshRes.error()};
        }
      }

      return proofRes;
    }
    catch (std::filesystem::filesystem_error const& error)
    {
      return makeError(Error::Code::IoError, error.what());
    }
  }

  Result<detail::ScanSpellingDirectory> ScanSpellingInspection::readStableNames(Path const& directory) const
  {
    auto firstRes = readScanNames(directory, _stopToken);

    if (!firstRes)
    {
      return firstRes;
    }

    auto const secondRes = readScanNames(directory, _stopToken);

    if (!secondRes)
    {
      return std::unexpected{secondRes.error()};
    }

    firstRes->retainStableGroups(*secondRes, _stopToken);
    return firstRes;
  }

  Result<detail::ScanSpellingDirectory> const& ScanSpellingInspection::stableNames(Path const& directory)
  {
    if (auto const entry = _directories.find(directory); entry != _directories.end())
    {
      return entry->second;
    }

    return _directories.emplace(directory, readStableNames(directory)).first->second;
  }

  Result<> ScanSpellingInspection::corroborateDirectorySingleton(Path const& parent,
                                                                 Path const& oldName,
                                                                 Path const& newName) const
  {
    auto const freshRes = readScanNames(parent, _stopToken);

    if (!freshRes)
    {
      return std::unexpected{freshRes.error()};
    }

    if (auto const freshMatchRes = freshRes->compareNames(oldName.native(), newName.native());
        !freshMatchRes || *freshMatchRes != detail::ScanSpellingDirectory::NameMatch::Singleton)
    {
      return makeError(Error::Code::Conflict, "Scan directory spelling entries changed during identity lookup");
    }

    return {};
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

  Result<detail::ScanSpellingDirectory::NameMatch> detail::ScanSpellingDirectory::compareNames(
    NativeName const& oldName,
    NativeName const& newName) const
  {
    auto const group = _groups.find(foldedName(newName));

    if (group == _groups.end() || !group->second.contains(newName))
    {
      return makeError(Error::Code::Conflict, "Expected scan spelling directory entry is missing or unstable");
    }

    if (group->second.contains(oldName))
    {
      return NameMatch::BothExact;
    }

    return group->second.size() == 1 ? NameMatch::Singleton : NameMatch::Ambiguous;
  }

  Result<ScanSpellingObservation> detail::ScanSpellingDirectory::observe(
    NativeName const& oldName,
    NativeName const& newName,
    std::function<Result<ScanSpellingLookup>()> const& lookup) const
  {
    auto const namesRes = compareNames(oldName, newName);

    if (!namesRes)
    {
      return std::unexpected{namesRes.error()};
    }

    // Two exact filenames are distinct entries, even hardlinks or pointers.
    if (*namesRes == NameMatch::BothExact)
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

    if (*namesRes == NameMatch::Ambiguous)
    {
      return makeError(Error::Code::Conflict, "Ambiguous scan spelling directory entries or file identity");
    }

    auto const& [oldFile, newFile] = std::get<ScanRegularFilePair>(*lookupRes);
    auto const identityRes = reconcileEntryIdentities(oldFile, newFile);

    if (!identityRes)
    {
      return std::unexpected{identityRes.error()};
    }

    if (auto const& optIdentity = *identityRes; optIdentity)
    {
      return *optIdentity;
    }

    return ScanSpellingNeedsFingerprint{};
  }
} // namespace ao::rt
