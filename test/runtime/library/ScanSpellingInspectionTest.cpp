// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanSpelling.h"
#include <ao/Error.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <variant>

namespace ao::rt::test
{
  namespace
  {
    struct SpellingSamples final
    {
      std::filesystem::path oldName = "File.flac";
      std::filesystem::path newName = "file.flac";
      std::filesystem::path peer = "FILE.flac";
      detail::ScanSpellingDirectory first{};
      detail::ScanSpellingDirectory second{};

      SpellingSamples()
      {
        first.add(newName.native());
        second.add(newName.native());
      }

      Result<ScanSpellingObservation> observe(Result<detail::ScanSpellingLookup> const& lookupRes)
      {
        first.retainStableGroups(second, {});
        return first.observe(oldName.native(), newName.native(), [&lookupRes] { return lookupRes; });
      }
    };
  } // namespace

  TEST_CASE("Scan spelling inspection - candidate keys fold only ASCII letters in the leaf")
  {
    auto const key = scanSpellingKey("Album/ÉA.flac");
    CHECK(key == scanSpellingKey("Album/Éa.flac"));
    CHECK(key != scanSpellingKey("Album/éa.flac"));
    CHECK(key != scanSpellingKey("Album/E\u0301a.flac"));
    CHECK(key != scanSpellingKey("album/Éa.flac"));
  }

  TEST_CASE("Scan directory spelling inspection - candidate keys and single component shape")
  {
    struct Sample final
    {
      std::string_view oldUri;
      std::string_view newUri;
      bool sameKey;
      bool pair;
    };

    for (auto const& sample : {Sample{"Album/x.flac", "album/x.flac", true, true},
                               Sample{"A/B/x.flac", "a/b/x.flac", true, false},
                               Sample{"A/B/x.flac", "A/b/x.flac", true, true},
                               Sample{"A/x.flac", "a/X.flac", false, false},
                               Sample{"ÉA/x.flac", "Éa/x.flac", true, true},
                               Sample{"É/x.flac", "é/x.flac", false, false},
                               Sample{"x.flac", "X.flac", false, false},
                               Sample{"A/x.flac", "a//x.flac", false, false}})
    {
      CAPTURE(sample.oldUri, sample.newUri);
      CHECK((scanDirectorySpellingKey(sample.oldUri) == scanDirectorySpellingKey(sample.newUri)) == sample.sameKey);
      CHECK(isScanDirectorySpellingPair(sample.oldUri, sample.newUri) == sample.pair);
    }
  }

  TEST_CASE("Scan directory spelling inspection - simulated identity fallback and conflict")
  {
    auto const missing = detail::ScanFileIdUnavailable{.optVolume = 17};
    auto const legacy = detail::ScanLegacyFileIndex{.volume = 17, .optFileIndex = 42};
    auto const directory = ScanFileIdentity{.volume = 17, .fileId = {1}};
    auto const file = ScanFileIdentity{.volume = 17, .fileId = {2}};
    auto const noneRes = detail::reconcileScanDirectorySpelling(missing, missing, missing, missing);
    REQUIRE(noneRes);
    REQUIRE(std::holds_alternative<ScanDirectorySpellingNeedsFingerprint>(*noneRes));
    auto const partialRes = detail::reconcileScanDirectorySpelling(directory, directory, legacy, legacy);
    REQUIRE(partialRes);
    REQUIRE(std::holds_alternative<ScanDirectorySpellingNeedsFingerprint>(*partialRes));
    auto const planned = std::get<ScanDirectorySpellingNeedsFingerprint>(*partialRes);
    CHECK(planned.optDirectory == directory);
    CHECK_FALSE(planned.optFile);
    CHECK(matchesScanDirectorySpellingFallback(planned, *partialRes));
    auto const changedRes = detail::reconcileScanDirectorySpelling(file, file, legacy, legacy);
    REQUIRE(changedRes);
    CHECK_FALSE(matchesScanDirectorySpellingFallback(planned, *changedRes));
    auto const upgraded = ScanSpellingObservation{ScanDirectorySpellingIdentity{.directory = directory, .file = file}};
    CHECK(matchesScanDirectorySpellingFallback(planned, upgraded));
    CHECK_FALSE(matchesScanDirectorySpellingFallback(planned, ScanDirectorySpellingNeedsFingerprint{}));
    CHECK_FALSE(matchesScanDirectorySpellingFallback(planned, DistinctScanSpellings{}));
    auto const fileOnly = ScanDirectorySpellingNeedsFingerprint{.optFile = file};
    CHECK(matchesScanDirectorySpellingFallback(fileOnly, upgraded));
    CHECK_FALSE(matchesScanDirectorySpellingFallback(fileOnly, *partialRes));
    CHECK_FALSE(matchesScanDirectorySpellingFallback(
      fileOnly, ScanDirectorySpellingIdentity{.directory = directory, .file = directory}));

    auto const conflictRes = detail::reconcileScanDirectorySpelling(directory, file, missing, missing);
    REQUIRE_FALSE(conflictRes);
    CHECK(conflictRes.error().code == Error::Code::Conflict);
    auto const wrongVolume = detail::ScanFileIdUnavailable{.optVolume = 23};
    auto const volumeConflictRes = detail::reconcileScanDirectorySpelling(missing, wrongVolume, missing, missing);
    REQUIRE_FALSE(volumeConflictRes);
    CHECK(volumeConflictRes.error().code == Error::Code::Conflict);
    auto const otherLegacy = detail::ScanLegacyFileIndex{.volume = 17, .optFileIndex = 43};
    auto const legacyConflictRes = detail::reconcileScanDirectorySpelling(legacy, otherLegacy, missing, missing);
    REQUIRE_FALSE(legacyConflictRes);
    CHECK(legacyConflictRes.error().code == Error::Code::Conflict);
    auto const mixedConflictRes = detail::reconcileScanDirectorySpelling(directory, legacy, missing, missing);
    REQUIRE_FALSE(mixedConflictRes);
    CHECK(mixedConflictRes.error().code == Error::Code::Conflict);
  }

  TEST_CASE("Scan spelling inspection - two unavailable file IDs require independent fingerprint evidence")
  {
    auto samples = SpellingSamples{};
    auto const files = detail::ScanRegularFilePair{
      .oldFile = detail::ScanFileIdUnavailable{.optVolume = 23}, .newFile = detail::ScanFileIdUnavailable{}};
    auto const observationRes = samples.observe(files);
    REQUIRE(observationRes);
    CHECK(std::holds_alternative<ScanSpellingNeedsFingerprint>(*observationRes));
  }

  TEST_CASE("Scan spelling inspection - matching legacy file indexes never replace fingerprint evidence")
  {
    auto samples = SpellingSamples{};

    for (auto const optIndex : {std::optional<std::uint64_t>{42}, std::optional<std::uint64_t>{}})
    {
      CAPTURE(optIndex);
      auto const hint = detail::ScanLegacyFileIndex{.volume = 17, .optFileIndex = optIndex};
      auto const observationRes = samples.observe(detail::ScanRegularFilePair{.oldFile = hint, .newFile = hint});
      REQUIRE(observationRes);
      CHECK(std::holds_alternative<ScanSpellingNeedsFingerprint>(*observationRes));
    }
  }

  TEST_CASE("Scan spelling inspection - equal native IDs retain the identity for revalidation")
  {
    auto samples = SpellingSamples{};
    auto const identity = ScanFileIdentity{.volume = 17, .fileId = {9}};
    auto const observationRes = samples.observe(detail::ScanRegularFilePair{.oldFile = identity, .newFile = identity});
    REQUIRE(observationRes);
    REQUIRE(std::holds_alternative<ScanFileIdentity>(*observationRes));
    CHECK(std::get<ScanFileIdentity>(*observationRes) == identity);
  }

  TEST_CASE("Scan spelling inspection - contradictory file evidence blocks fingerprint fallback")
  {
    auto samples = SpellingSamples{};
    auto const identity = ScanFileIdentity{.volume = 17, .fileId = {9}};
    auto files = detail::ScanRegularFilePair{.oldFile = identity, .newFile = identity};

    SECTION("different file IDs on the same volume")
    {
      files.newFile = ScanFileIdentity{.volume = 17, .fileId = {5}};
    }

    SECTION("only the destination has an ID")
    {
      files.oldFile = detail::ScanFileIdUnavailable{};
    }

    SECTION("only the source has an ID")
    {
      files.newFile = detail::ScanFileIdUnavailable{};
    }

    SECTION("legacy indexes differ on the same volume")
    {
      files.oldFile = detail::ScanLegacyFileIndex{.volume = 17, .optFileIndex = 42};
      files.newFile = detail::ScanLegacyFileIndex{.volume = 17, .optFileIndex = 43};
    }

    SECTION("matching legacy indexes on different volumes")
    {
      files.oldFile = detail::ScanLegacyFileIndex{.volume = 17, .optFileIndex = 42};
      files.newFile = detail::ScanLegacyFileIndex{.volume = 23, .optFileIndex = 42};
    }

    SECTION("only the destination has a legacy hint rather than a native ID")
    {
      files.newFile = detail::ScanLegacyFileIndex{.volume = 17, .optFileIndex = 9};
    }

    SECTION("only the source has a legacy hint rather than a native ID")
    {
      files.oldFile = detail::ScanLegacyFileIndex{.volume = 17, .optFileIndex = 9};
    }

    SECTION("unavailable IDs on different known volumes")
    {
      files.oldFile = detail::ScanFileIdUnavailable{.optVolume = 17};
      files.newFile = detail::ScanFileIdUnavailable{.optVolume = 23};
    }

    auto const observationRes = samples.observe(files);
    REQUIRE_FALSE(observationRes);
    CHECK(observationRes.error().code == Error::Code::Conflict);
  }

#ifdef _WIN32
  TEST_CASE("Scan spelling inspection - an invalid FileIdInfo parameter requires a successful legacy lookup")
  {
    constexpr std::uint32_t kInvalidParameter = 87;
    auto const hint = detail::ScanLegacyFileIndex{.volume = 31, .optFileIndex = 71};
    auto legacyRes = Result<detail::ScanLegacyFileIndex>{hint};
    std::uint32_t calls = 0;
    auto const readLegacy = [&]
    {
      ++calls;
      return legacyRes;
    };

    SECTION("preserve the hint but still require a fingerprint")
    {
      auto const fileRes = detail::recoverWindowsScanFileId(kInvalidParameter, readLegacy);
      REQUIRE(fileRes);
      REQUIRE(std::holds_alternative<detail::ScanLegacyFileIndex>(*fileRes));
      CHECK(std::get<detail::ScanLegacyFileIndex>(*fileRes) == hint);
      auto samples = SpellingSamples{};
      auto const observationRes = samples.observe(detail::ScanRegularFilePair{.oldFile = hint, .newFile = *fileRes});
      REQUIRE(observationRes);
      CHECK(std::holds_alternative<ScanSpellingNeedsFingerprint>(*observationRes));
    }

    SECTION("propagate the legacy error rather than dropping identity evidence")
    {
      legacyRes = makeError(Error::Code::Conflict);
      auto const fileRes = detail::recoverWindowsScanFileId(kInvalidParameter, readLegacy);
      REQUIRE_FALSE(fileRes);
      CHECK(fileRes.error().code == Error::Code::Conflict);
    }

    CHECK(calls == 1);
  }

  TEST_CASE("Scan spelling inspection - other FileIdInfo errors never query a legacy index")
  {
    constexpr std::uint32_t kInvalidFunction = 1;
    constexpr std::uint32_t kNotSupported = 50;
    constexpr std::uint32_t kAccessDenied = 5;
    std::uint32_t calls = 0;
    auto const readLegacy = [&calls] -> Result<detail::ScanLegacyFileIndex>
    {
      ++calls;
      return detail::ScanLegacyFileIndex{.volume = 31, .optFileIndex = 71};
    };

    for (auto const code : {kInvalidFunction, kNotSupported})
    {
      CAPTURE(code);
      auto const fileRes = detail::recoverWindowsScanFileId(code, readLegacy);
      REQUIRE(fileRes);
      CHECK(std::holds_alternative<detail::ScanFileIdUnavailable>(*fileRes));
    }

    auto const deniedRes = detail::recoverWindowsScanFileId(kAccessDenied, readLegacy);
    REQUIRE_FALSE(deniedRes);
    CHECK(deniedRes.error().code == Error::Code::IoError);
    CHECK(calls == 0);
  }
#endif

  TEST_CASE("Scan spelling inspection - failed lookups are not unavailable regular-file IDs")
  {
    auto samples = SpellingSamples{};
    auto code = Error::Code::IoError;

    SECTION("I/O failure")
    {
    }

    SECTION("unsupported operation other than the ID query")
    {
      code = Error::Code::NotSupported;
    }

    auto const observationRes = samples.observe(makeError(code));
    REQUIRE_FALSE(observationRes);
    CHECK(observationRes.error().code == code);
  }

  TEST_CASE("Scan spelling inspection - distinct entries need no usable file ID")
  {
    auto samples = SpellingSamples{};
    auto lookupRes = Result<detail::ScanSpellingLookup>{DistinctScanSpellings{}};

    SECTION("the old lookup was absent and the destination still exists")
    {
    }

    SECTION("both exact raw names are enumerated")
    {
      samples.first.add(samples.oldName.native());
      samples.second.add(samples.oldName.native());
      lookupRes = makeError(Error::Code::IoError);
    }

    auto const observationRes = samples.observe(lookupRes);
    REQUIRE(observationRes);
    CHECK(std::holds_alternative<DistinctScanSpellings>(*observationRes));
  }

  TEST_CASE("Scan spelling inspection - a third or unstable spelling blocks fallback")
  {
    auto samples = SpellingSamples{};
    samples.second.add(samples.peer.native());

    SECTION("a matching third spelling in both listings")
    {
      samples.first.add(samples.peer.native());
    }

    SECTION("a third spelling appears between listings")
    {
    }

    auto const observationRes = samples.observe(detail::ScanRegularFilePair{
      .oldFile = detail::ScanFileIdUnavailable{}, .newFile = detail::ScanFileIdUnavailable{}});
    REQUIRE_FALSE(observationRes);
    CHECK(observationRes.error().code == Error::Code::Conflict);
  }
} // namespace ao::rt::test
