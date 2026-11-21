// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace ao::rt
{
  // Native identity observed during this scan, never persisted or used as an audio fingerprint.
  struct ScanFileIdentity final
  {
    std::uint64_t volume = 0;
    std::array<std::uint8_t, 16> fileId{};
    bool operator==(ScanFileIdentity const&) const noexcept = default;
  };

  struct DistinctScanSpellings final
  {
    bool operator==(DistinctScanSpellings const&) const noexcept = default;
  };

  struct ScanSpellingNeedsFingerprint final
  {
    bool operator==(ScanSpellingNeedsFingerprint const&) const noexcept = default;
  };

  using ScanSpellingObservation = std::variant<DistinctScanSpellings, ScanFileIdentity, ScanSpellingNeedsFingerprint>;

  struct ScanSpellingEvidence final
  {
    std::unordered_map<std::string, ScanSpellingObservation> byDestination{};
  };

  namespace detail
  {
    // Preserve a known volume even when the regular file has no usable ID.
    struct ScanFileIdUnavailable final
    {
      std::optional<std::uint64_t> optVolume{};
    };
    // A legacy Windows index can contradict a pairing, but cannot prove
    // identity once the handles are closed between scan phases.
    struct ScanLegacyFileIndex final
    {
      std::uint32_t volume = 0;
      std::optional<std::uint64_t> optFileIndex{};
      bool operator==(ScanLegacyFileIndex const&) const noexcept = default;
    };
    using ScanRegularFile = std::variant<ScanFileIdentity, ScanFileIdUnavailable, ScanLegacyFileIndex>;

#ifdef _WIN32
    Result<ScanRegularFile> recoverWindowsScanFileId(std::uint32_t code,
                                                     std::function<Result<ScanLegacyFileIndex>()> const& readLegacy);
#endif

    struct ScanRegularFilePair final
    {
      ScanRegularFile oldFile;
      ScanRegularFile newFile;
    };
    using ScanSpellingLookup = std::variant<DistinctScanSpellings, ScanRegularFilePair>;

    class ScanSpellingDirectory final
    {
    public:
      using NativeName = std::filesystem::path::string_type;
      void add(NativeName const& name);
      void retainStableGroups(ScanSpellingDirectory const& second, std::stop_token stopToken);
      Result<ScanSpellingObservation> observe(NativeName const& oldName,
                                              NativeName const& newName,
                                              std::function<Result<ScanSpellingLookup>()> const& lookup) const;

    private:
      std::unordered_map<NativeName, std::set<NativeName>> _groups{};
    };
  } // namespace detail

  // Fold ASCII letters in the leaf only. Equal keys are candidates, not proof.
  std::string scanSpellingKey(std::string_view uri);

  // Construct afresh for each planning, preparation or pretransaction phase.
  // Only raw directory names are cached; file identities are always queried live.
  class ScanSpellingInspection final
  {
  public:
    ScanSpellingInspection(std::filesystem::path root, std::stop_token stopToken);

    // For a same-parent ASCII case-only rename candidate. NeedsFingerprint is
    // not proof of identity: both regular leaves lacked usable native IDs.
    Result<ScanSpellingObservation> inspect(std::string_view oldUri, std::string_view newUri);

  private:
    Result<detail::ScanSpellingDirectory> readNames(std::filesystem::path const& directory) const;
    Result<detail::ScanSpellingDirectory> readStableNames(std::filesystem::path const& directory) const;

    std::filesystem::path _root;
    std::stop_token _stopToken;
    std::unordered_map<std::filesystem::path, Result<detail::ScanSpellingDirectory>> _directories{};
  };
} // namespace ao::rt
