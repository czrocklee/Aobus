// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/audio/AudioFixtureSupport.h"
#include "test/media/wav/TestWav.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileStatus.h>
#include <ao/library/FileTimeConversion.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/AudioFingerprintReport.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kSongUri = "song.flac";
    constexpr auto kCarrierUri = "disc.wav";
    constexpr std::size_t kCarrierFrames = 1764;
    constexpr std::uint32_t kCarrierRate = 44100;
    constexpr std::size_t kBytesPerFrame = 2; // Mono, signed 16-bit PCM.
    constexpr std::size_t kFirstSegmentFrames = 588;
    constexpr auto kFirstSegment =
      library::TrackSegment{.startFrame = 0, .frameCount = kFirstSegmentFrames, .ordinal = 1};

    struct FingerprintFixture final
    {
      MusicLibraryFixture music;
      QueuedExecutor executor;
      LibraryChanges changes = makeLibraryChanges(executor, music.library());
      LibraryCommandsFixture runtime{music.library(), changes, executor};

      std::vector<TrackId> importFiles(AudioIdentityPolicy policy = AudioIdentityPolicy::DeferNew)
      {
        auto planRes = LibraryScan{music.library()}.buildPlan();
        REQUIRE(planRes);
        auto res = runtime.runTask(
          jobs().applyScanPlanAsync(std::move(*planRes), ScanApplyOptions{.audioIdentityPolicy = policy}));
        REQUIRE(res);
        REQUIRE(res->failureCount == 0);
        return std::move(res->insertedIds);
      }

      LibraryJobs& jobs() { return runtime.library().jobs(); }

      std::uint64_t revision() const { return music.library().libraryRevision(music.library().readTransaction()); }

      library::AudioIdentity identity(std::string_view uri) const
      {
        auto const read = music.library().readTransaction();
        auto const optRow = music.library().manifest().reader(read).get(uri);
        REQUIRE(optRow);
        return {.signature = optRow->audioSignature(), .payloadLength = optRow->audioPayloadLength()};
      }
    };

    void writeCarrier(std::filesystem::path const& path,
                      std::uint32_t rate,
                      std::size_t frames,
                      std::uint8_t sampleByte = 0)
    {
      REQUIRE(frames <= kCarrierFrames);
      // Compensate for the PCM size change so the file-size guard cannot
      // mask an extent mismatch. JUNK does not contribute to the PCM identity.
      auto const removedPcmBytes = (kCarrierFrames - frames) * kBytesPerFrame;
      auto const bytes = ao::test::wav::makeWav({
        .sampleRate = rate,
        .audioData = std::vector<std::uint8_t>(frames * kBytesPerFrame, sampleByte),
        .extraChunks = {{{'J', 'U', 'N', 'K'}, std::vector<std::uint8_t>(16 + removedPcmBytes)}},
      });
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      output.close();
      REQUIRE(output);
    }

    void writeCarrierSheet(std::filesystem::path const& root)
    {
      auto sheet = std::ofstream{root / "disc.cue", std::ios::binary};
      sheet << "FILE \"disc.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
               "TRACK 02 AUDIO\nINDEX 01 00:00:01\n";
      sheet.close();
      REQUIRE(sheet);
    }
  } // namespace

  TEST_CASE("Audio fingerprint verification - adopting a pending identity publishes one manifest-only revision")
  {
    auto fixture = FingerprintFixture{};
    std::filesystem::copy_file(
      audio::test::requireAudioFixture("basic_metadata.flac"), fixture.music.root() / kSongUri);
    auto const ids = fixture.importFiles();
    REQUIRE(ids.size() == 1);
    REQUIRE(fixture.runtime.updateMetadata(ids, MetadataPatch{.optTitle = "Curated song"}));
    auto const before = fixture.revision();
    auto publications = std::vector<LibraryChangeSet>{};
    auto const sub =
      fixture.changes.onChanged([&publications](LibraryChangeSet const& change) { publications.push_back(change); });

    auto const previewRes = fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri));

    REQUIRE(previewRes);
    CHECK(previewRes->uri == kSongUri);
    CHECK(previewRes->status == AudioFingerprintStatus::Pending);
    CHECK_FALSE(previewRes->updated);
    CHECK_FALSE(previewRes->optStored);
    REQUIRE(previewRes->optConfirmation);
    REQUIRE_FALSE(previewRes->optConfirmation->empty());
    CHECK(library::hasAudioIdentity(previewRes->observed.payloadLength, previewRes->observed.signature));
    CHECK(fixture.identity(kSongUri).payloadLength == 0);
    CHECK(fixture.revision() == before);
    CHECK(publications.empty());

    auto const adoptRes =
      fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri, previewRes->optConfirmation));

    REQUIRE(adoptRes);
    CHECK(adoptRes->status == AudioFingerprintStatus::Pending);
    CHECK(adoptRes->updated);
    CHECK_FALSE(adoptRes->optStored);
    CHECK_FALSE(adoptRes->optConfirmation);
    auto const expectedPublication = std::vector{LibraryChangeSet{.libraryRevision = before + 1}};
    CHECK(publications == expectedPublication);

    auto const replayRes =
      fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri, previewRes->optConfirmation));

    REQUIRE_FALSE(replayRes);
    CHECK(replayRes.error().code == Error::Code::Conflict);
    CHECK(publications == expectedPublication);
    CHECK(fixture.revision() == before + 1);
    auto const stored = fixture.identity(kSongUri);
    CHECK(stored.payloadLength == previewRes->observed.payloadLength);
    CHECK(stored.signature == previewRes->observed.signature);
    auto const& library = fixture.music.library();
    auto const read = library.readTransaction();
    auto const optRow = library.manifest().reader(read).get(kSongUri);
    REQUIRE(optRow);
    CHECK(optRow->trackId() == ids.front());
    auto const tracks = library.tracks().reader(read);
    CHECK(tracks.entryCount() == 1);
    auto const optTrack = tracks.get(ids.front());
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == kSongUri);
    CHECK(optTrack->metadata().title() == "Curated song");
  }

  TEST_CASE("Audio fingerprint verification - stale confirmation does not write or publish")
  {
    auto fixture = FingerprintFixture{};
    auto const path = fixture.music.root() / kSongUri;
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), path);
    REQUIRE(fixture.importFiles().size() == 1);
    auto const previewRes = fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri));
    REQUIRE(previewRes);
    REQUIRE(previewRes->optConfirmation);
    REQUIRE_FALSE(previewRes->optConfirmation->empty());

    SECTION("an unrelated Track creation advances the library revision")
    {
      auto const before = fixture.revision();
      auto const uri = audio::test::installAudioFixture(fixture.music.root(), "hires.flac", "other.flac");
      REQUIRE(fixture.runtime.runTask(fixture.runtime.commands().createTrackFromFileAsync(uri)));
      REQUIRE(fixture.revision() == before + 1);
    }

    SECTION("the target mtime no longer matches the manifest")
    {
      auto const writeTime = std::filesystem::last_write_time(path);
      std::filesystem::last_write_time(path, writeTime + std::chrono::seconds{10});
    }

    auto const beforeConfirm = fixture.revision();
    auto publications = std::vector<LibraryChangeSet>{};
    auto const sub =
      fixture.changes.onChanged([&publications](LibraryChangeSet const& change) { publications.push_back(change); });
    auto const res =
      fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri, previewRes->optConfirmation));

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::Conflict);
    CHECK(publications.empty());
    CHECK(fixture.revision() == beforeConfirm);
    CHECK(fixture.identity(kSongUri).payloadLength == 0);
  }

  TEST_CASE("Audio fingerprint verification - adopting changed carrier audio preserves member Tracks and file facts")
  {
    auto fixture = FingerprintFixture{};
    auto const path = fixture.music.root() / kCarrierUri;
    writeCarrier(path, kCarrierRate, kCarrierFrames);
    writeCarrierSheet(fixture.music.root());
    auto const members = fixture.importFiles(AudioIdentityPolicy::Eager);
    REQUIRE(members.size() == 2);
    REQUIRE(fixture.runtime.updateMetadata(std::span{members}.first(1), MetadataPatch{.optTitle = "Curated first"}));
    REQUIRE(fixture.runtime.updateMetadata(std::span{members}.last(1), MetadataPatch{.optTitle = "Curated second"}));
    auto const prior = fixture.identity(kCarrierUri);
    REQUIRE(prior.payloadLength == kCarrierFrames * kBytesPerFrame);
    auto expectedMembers = std::span{members};

    auto const deleteLastMember = GENERATE(false, true);
    CAPTURE(deleteLastMember);

    if (deleteLastMember)
    {
      REQUIRE(fixture.runtime.runTask(fixture.runtime.commands().deleteTrackAsync(members.back())));
      expectedMembers = expectedMembers.first(1);
    }

    auto const before = fixture.revision();
    auto const size = std::filesystem::file_size(path);
    auto const writeTime = std::filesystem::last_write_time(path);
    auto const mtimeRes = library::fileTimestampFromFileTime(writeTime);
    REQUIRE(mtimeRes);
    auto publications = std::vector<LibraryChangeSet>{};
    auto const sub =
      fixture.changes.onChanged([&publications](LibraryChangeSet const& change) { publications.push_back(change); });

    writeCarrier(path, kCarrierRate, kCarrierFrames, 1);
    std::filesystem::last_write_time(path, writeTime);
    REQUIRE(std::filesystem::file_size(path) == size);
    REQUIRE((std::filesystem::last_write_time(path) == writeTime));

    auto const previewRes = fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kCarrierUri));
    REQUIRE(previewRes);
    CHECK(previewRes->status == AudioFingerprintStatus::Mismatch);
    CHECK_FALSE(previewRes->updated);
    REQUIRE(previewRes->optStored);
    CHECK(previewRes->optStored->signature == prior.signature);
    CHECK(previewRes->optStored->payloadLength == prior.payloadLength);
    CHECK(previewRes->observed.payloadLength == prior.payloadLength);
    REQUIRE(previewRes->observed.signature != prior.signature);
    REQUIRE(previewRes->optConfirmation);
    REQUIRE_FALSE(previewRes->optConfirmation->empty());
    CHECK(fixture.identity(kCarrierUri).signature == prior.signature);
    CHECK(fixture.revision() == before);
    CHECK(publications.empty());

    auto const adoptRes =
      fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kCarrierUri, previewRes->optConfirmation));
    REQUIRE(adoptRes);
    CHECK(adoptRes->status == AudioFingerprintStatus::Mismatch);
    CHECK(adoptRes->updated);
    CHECK_FALSE(adoptRes->optConfirmation);
    CHECK(fixture.revision() == before + 1);
    CHECK(publications == std::vector{LibraryChangeSet{.libraryRevision = before + 1}});

    auto const& storage = fixture.music.library();
    auto const read = storage.readTransaction();
    auto const optRow = storage.manifest().reader(read).get(kCarrierUri);
    REQUIRE(optRow);
    CHECK(optRow->audioSignature() == previewRes->observed.signature);
    CHECK(optRow->audioPayloadLength() == previewRes->observed.payloadLength);
    CHECK(optRow->status() == library::FileStatus::Available);
    CHECK(optRow->fileSize() == size);
    CHECK(optRow->mtime() == *mtimeRes);
    CHECK(optRow->trackId() == kInvalidTrackId);
    CHECK(optRow->totalFrames() == kCarrierFrames);
    CHECK(optRow->segmentCount() == expectedMembers.size());
    CHECK(std::ranges::equal(optRow->segmentTrackIds(), expectedMembers));

    auto const tracks = storage.tracks().reader(read);
    CHECK(tracks.entryCount() == expectedMembers.size());
    auto const checkMember = [&](TrackId id, std::string_view title, library::TrackSegment segment)
    {
      auto const optTrack = tracks.get(id);
      REQUIRE(optTrack);
      CHECK(optTrack->metadata().title() == title);
      CHECK(optTrack->property().uri() == kCarrierUri);
      CHECK(optTrack->property().sampleRate() == SampleRate{kCarrierRate});
      CHECK(optTrack->property().segment() == segment);
    };
    checkMember(members.front(), "Curated first", kFirstSegment);

    if (expectedMembers.size() == 2)
    {
      checkMember(
        members.back(), "Curated second", {.startFrame = kFirstSegmentFrames, .frameCount = 1176, .ordinal = 2});
    }
  }

  TEST_CASE("Audio fingerprint verification - carrier extent and rate are required even when stats match")
  {
    auto fixture = FingerprintFixture{};
    auto const path = fixture.music.root() / kCarrierUri;
    writeCarrier(path, kCarrierRate, kCarrierFrames);
    writeCarrierSheet(fixture.music.root());

    auto const members = fixture.importFiles();
    REQUIRE(members.size() == 2);
    auto const& storage = fixture.music.library();
    auto expectedMembers = std::span{members};
    auto rate = kCarrierRate;
    auto frames = kCarrierFrames;

    SECTION("the PCM extent changes without changing the sample rate")
    {
      frames = 1176;
    }

    SECTION("the sample rate changes without changing the PCM")
    {
      rate = 48000;
    }

    SECTION("the PCM is shortened to the sole surviving segment's end")
    {
      REQUIRE(fixture.runtime.runTask(fixture.runtime.commands().deleteTrackAsync(members.back())));
      expectedMembers = expectedMembers.first(1);
      auto const read = storage.readTransaction();
      auto const optRow = storage.manifest().reader(read).get(kCarrierUri);
      REQUIRE(optRow);
      REQUIRE(std::ranges::equal(optRow->segmentTrackIds(), expectedMembers));
      auto const optTrack = storage.tracks().reader(read).get(members.front());
      REQUIRE(optTrack);
      REQUIRE(optTrack->property().segment() == kFirstSegment);
      frames = kFirstSegmentFrames;
    }

    auto const previewRes = fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kCarrierUri));
    REQUIRE(previewRes);
    REQUIRE(previewRes->optConfirmation);
    REQUIRE_FALSE(previewRes->optConfirmation->empty());
    auto const before = fixture.revision();
    auto const size = std::filesystem::file_size(path);
    auto const writeTime = std::filesystem::last_write_time(path);
    writeCarrier(path, rate, frames);
    std::filesystem::last_write_time(path, writeTime);
    REQUIRE(std::filesystem::file_size(path) == size);
    REQUIRE((std::filesystem::last_write_time(path) == writeTime));
    auto const verifyRes = fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kCarrierUri));
    REQUIRE_FALSE(verifyRes);
    CHECK(verifyRes.error().code == Error::Code::Conflict);
    CHECK(verifyRes.error().message.contains("Carrier extent or sample rate"));

    auto const confirmRes =
      fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kCarrierUri, previewRes->optConfirmation));
    REQUIRE_FALSE(confirmRes);
    CHECK(confirmRes.error().code == Error::Code::Conflict);
    CHECK(confirmRes.error().message.contains("Carrier extent or sample rate"));
    CHECK(fixture.revision() == before);
    auto const read = storage.readTransaction();
    auto const optRow = storage.manifest().reader(read).get(kCarrierUri);
    REQUIRE(optRow);
    CHECK(optRow->audioSignature() == library::AudioIdentity{}.signature);
    CHECK(optRow->audioPayloadLength() == 0);
    CHECK(optRow->totalFrames() == kCarrierFrames);
    CHECK(std::ranges::equal(optRow->segmentTrackIds(), expectedMembers));
  }
} // namespace ao::rt::test
