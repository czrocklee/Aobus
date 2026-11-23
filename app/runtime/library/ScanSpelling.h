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

  struct ScanDirectorySpellingIdentity final
  {
    ScanFileIdentity directory;
    ScanFileIdentity file;
    bool operator==(ScanDirectorySpellingIdentity const&) const noexcept = default;
  };

  // Directory fallback still requires a unique audio match. Retain any agreed native ID across phases.
  struct ScanDirectorySpellingNeedsFingerprint final
  {
    std::optional<ScanFileIdentity> optDirectory{};
    std::optional<ScanFileIdentity> optFile{};
    bool operator==(ScanDirectorySpellingNeedsFingerprint const&) const noexcept = default;
  };

  using ScanSpellingObservation = std::variant<DistinctScanSpellings,
                                               ScanFileIdentity,
                                               ScanSpellingNeedsFingerprint,
                                               ScanDirectorySpellingIdentity,
                                               ScanDirectorySpellingNeedsFingerprint>;

  bool matchesScanDirectorySpellingFallback(ScanDirectorySpellingNeedsFingerprint const& planned,
                                            ScanSpellingObservation const& live) noexcept;

  struct ScanSpellingEvidence final
  {
    std::unordered_map<std::string, ScanSpellingObservation> byDestination{};
  };

  namespace detail
  {
    // Preserve a known volume even when the entry has no usable ID.
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
    using ScanNativeIdentity = std::variant<ScanFileIdentity, ScanFileIdUnavailable, ScanLegacyFileIndex>;

#ifdef _WIN32
    Result<ScanNativeIdentity> recoverWindowsScanFileId(std::uint32_t code,
                                                        std::function<Result<ScanLegacyFileIndex>()> const& readLegacy);
#endif

    // Reconcile IDs only after the caller has proved a stable singleton and direct lookups.
    Result<ScanSpellingObservation> reconcileScanDirectorySpelling(ScanNativeIdentity const& oldDirectory,
                                                                   ScanNativeIdentity const& newDirectory,
                                                                   ScanNativeIdentity const& oldFile,
                                                                   ScanNativeIdentity const& newFile);

    struct ScanRegularFilePair final
    {
      ScanNativeIdentity oldFile;
      ScanNativeIdentity newFile;
    };
    using ScanSpellingLookup = std::variant<DistinctScanSpellings, ScanRegularFilePair>;

    class ScanSpellingDirectory final
    {
    public:
      using NativeName = std::filesystem::path::string_type;
      enum class NameMatch : std::uint8_t
      {
        BothExact,
        Singleton,
        Ambiguous,
      };

      void add(NativeName const& name);
      void retainStableGroups(ScanSpellingDirectory const& second, std::stop_token stopToken);
      Result<NameMatch> compareNames(NativeName const& oldName, NativeName const& newName) const;
      Result<ScanSpellingObservation> observe(NativeName const& oldName,
                                              NativeName const& newName,
                                              std::function<Result<ScanSpellingLookup>()> const& lookup) const;

    private:
      std::unordered_map<NativeName, std::set<NativeName>> _groups{};
    };
  } // namespace detail

  // Fold ASCII letters in the leaf only. Equal keys are candidates, not proof.
  std::string scanSpellingKey(std::string_view uri);

  // Fold ASCII letters in parent components, keeping the basename byte-exact.
  std::string scanDirectorySpellingKey(std::string_view uri);
  // Check the single-component shape of two canonical URIs, not filesystem identity.
  bool isScanDirectorySpellingPair(std::string_view oldUri, std::string_view newUri) noexcept;

  // For Missing scan items only: a verified direct regular ancestor makes its descendant absent.
  // Throws async::OperationCancelled on a stop request.
  bool hasExactRegularScanAncestor(std::filesystem::path const& root, std::string_view uri, std::stop_token stopToken);

  enum class ScanDirectoryProbe : std::uint8_t
  {
    SpellingCandidate,
    IndependentPeer,
  };

  // Construct afresh for each planning, preparation or pretransaction phase.
  // Only raw directory names are cached; identities are always queried live.
  class ScanSpellingInspection final
  {
  public:
    ScanSpellingInspection(std::filesystem::path root, std::stop_token stopToken);

    // For a same-parent ASCII case-only rename candidate. NeedsFingerprint is
    // not proof of identity: both regular leaves lacked usable native IDs.
    Result<ScanSpellingObservation> inspect(std::string_view oldUri, std::string_view newUri);

    // For one ASCII case-only parent component with the rest of the URI unchanged.
    // A live alias needs reliable directory and file IDs or independent fingerprint evidence.
    // An IndependentPeer proof also requires nonredirected suffixes on any traversable arm.
    Result<ScanSpellingObservation> inspectDirectory(std::string_view oldUri,
                                                     std::string_view newUri,
                                                     ScanDirectoryProbe probe = ScanDirectoryProbe::SpellingCandidate);

  private:
    Result<detail::ScanSpellingDirectory> readNames(std::filesystem::path const& directory) const;
    Result<detail::ScanSpellingDirectory> readStableNames(std::filesystem::path const& directory) const;
    Result<detail::ScanSpellingDirectory> const& stableNames(std::filesystem::path const& directory);
    Result<> corroborateDirectorySingleton(std::filesystem::path const& parent,
                                           std::filesystem::path const& oldName,
                                           std::filesystem::path const& newName) const;

    std::filesystem::path _root;
    std::stop_token _stopToken;
    std::unordered_map<std::filesystem::path, Result<detail::ScanSpellingDirectory>> _directories{};
  };
} // namespace ao::rt
