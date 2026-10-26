// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "test/runtime/library/ScanApplyTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/utility/Path.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::filesystem::path copyFixtureAudio(MusicLibraryFixture const& libraryFixture, std::filesystem::path const& name)
    {
      auto const source = std::filesystem::path{AUDIO_TEST_DATA_DIR} / "empty.flac";

      INFO("Required audio fixture: " << source);
      REQUIRE(std::filesystem::is_regular_file(source));

      auto const destination = libraryFixture.root() / name;
      std::filesystem::create_directories(destination.parent_path());
      std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
      return destination;
    }

    constexpr auto kCueLayout = "FILE \"image.flac\" WAVE\n"
                                "TRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                                "TRACK 02 AUDIO\nINDEX 01 00:01:00\n";

    void writeCueSheet(std::filesystem::path const& path, std::string_view text)
    {
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output << text;
      REQUIRE(output.good());
    }

    template<typename Reply, typename Handoff>
    Result<Reply> runImportAcrossAdmission(ManualExecutor& executor, async::Task<Result<Reply>> task, Handoff handoff)
    {
      // The production Asio executor exposes the idle lane's mandatory grant
      // post: preparation finishes on the first turn, admission on the next.
      auto context = boost::asio::io_context{};
      auto future = boost::asio::co_spawn(context, std::move(task), boost::asio::use_future);
      REQUIRE(context.poll_one() == 1);
      REQUIRE(executor.queuedCount() == 0);
      handoff();

      REQUIRE(executor.tryDrainUntil(
        [&]
        {
          context.restart();
          std::ignore = context.poll();
          return future.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
        }));
      return future.get();
    }

    std::filesystem::path createTextFile(MusicLibraryFixture const& libraryFixture, std::string const& name)
    {
      auto const destination = libraryFixture.root() / name;
      auto out = std::ofstream{destination};
      out << "not audio";
      return destination;
    }

    // Components stay below filesystem name limits while the path grows past
    // the 1,976-byte URI limit. Ten stay within Linux's 4,096-byte path limit
    // and resolve; twenty-one exceed it, so there resolution fails and the
    // path is measured lexically.
    std::filesystem::path longTrackPath(std::filesystem::path path, std::size_t const componentCount)
    {
      for (std::size_t index = 0; index < componentCount; ++index)
      {
        path /= std::string(200, 'a');
      }

      return path / "track.flac";
    }
  } // namespace

  TEST_CASE("LibraryCommands - createTrackFromFile imports a valid file and publishes a mutation")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });
    auto inserted = std::vector<TrackId>{};
    auto collectionSub = changes.onChanged([&](LibraryChangeSet const& ev) noexcept { inserted = ev.tracksInserted; });

    auto const absValidFile = copyFixtureAudio(libraryFixture, "music/song.flac");
    REQUIRE(std::filesystem::is_regular_file(absValidFile));

    auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(absValidFile));
    REQUIRE(trackIdRes);
    CHECK(mutated.empty());
    REQUIRE(inserted.size() == 1);
    CHECK(inserted[0] == trackIdRes->trackId);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrackView = libraryFixture.library()
                                .tracks()
                                .reader(transaction)
                                .get(trackIdRes->trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrackView);
    CHECK(optTrackView->property().uri() == "music/song.flac");
    CHECK(libraryFixture.library().manifest().reader(transaction).get("music/song.flac"));

    auto const duplicateRes = commandsFixture.runTask(commands.createTrackFromFileAsync(absValidFile));
    REQUIRE(!duplicateRes);
    CHECK(duplicateRes.error().code == Error::Code::Conflict);
    CHECK(duplicateRes.error().message.contains("already imported"));
  }

  TEST_CASE("LibraryCommands - add-file rejects a malformed sheet inserted during lane handoff", "[concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto& commands = commandsFixture.commands();
    auto const audio = copyFixtureAudio(libraryFixture, "song.flac");
    auto const revisionBefore = [&]
    {
      auto transaction = libraryFixture.library().readTransaction();
      return libraryFixture.library().libraryRevision(transaction);
    }();
    std::size_t publicationCount = 0;
    auto sub = changes.onChanged([&](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto const insertSheet = [&] { writeCueSheet(libraryFixture.root() / "song.CuE", "not a CUE sheet"); };

    SECTION("commit")
    {
      auto const res = runImportAcrossAdmission(executor, commands.createTrackFromFileAsync(audio), insertSheet);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::Conflict);
      CHECK(res.error().message.contains("CUE sheets changed"));
    }

    SECTION("preview")
    {
      auto const res = runImportAcrossAdmission(executor, commands.previewCreateTrackFromFileAsync(audio), insertSheet);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::Conflict);
      CHECK(res.error().message.contains("CUE sheets changed"));
    }

    CHECK(publicationCount == 0);
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == revisionBefore);
    CHECK(libraryFixture.library().tracks().reader(transaction).entryCount() == 0);
    CHECK_FALSE(libraryFixture.library().manifest().reader(transaction).get("song.flac"));
  }

  TEST_CASE("LibraryCommands - add-file rejects changed CUE ranges even with unchanged sheet size and time",
            "[concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto& commands = commandsFixture.commands();
    auto const audio = libraryFixture.root() / "image.flac";
    writeScanFlacMetadataFixture(audio, {"TITLE=Carrier"}, 88200);
    auto const sheet = libraryFixture.root() / "image.cue";
    writeCueSheet(sheet, kCueLayout);
    auto const originalSize = std::filesystem::file_size(sheet);
    auto const originalTime = std::filesystem::last_write_time(sheet);
    auto const revisionBefore = [&]
    {
      auto transaction = libraryFixture.library().readTransaction();
      return libraryFixture.library().libraryRevision(transaction);
    }();
    auto publications = std::vector<LibraryChangeSet>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& change) noexcept { publications.push_back(change); });

    auto const res = runImportAcrossAdmission(executor,
                                              commands.createTrackFromFileAsync(audio),
                                              [&]
                                              {
                                                writeCueSheet(sheet,
                                                              "FILE \"image.flac\" WAVE\n"
                                                              "TRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                                                              "TRACK 02 AUDIO\nINDEX 01 00:00:50\n");
                                                std::filesystem::last_write_time(sheet, originalTime);
                                                REQUIRE(std::filesystem::file_size(sheet) == originalSize);
                                                REQUIRE(std::filesystem::last_write_time(sheet) == originalTime);
                                              });

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::Conflict);
    CHECK(res.error().message.contains("CUE sheets changed"));
    CHECK(publications.empty());

    {
      auto transaction = libraryFixture.library().readTransaction();
      CHECK(libraryFixture.library().libraryRevision(transaction) == revisionBefore);
      CHECK(libraryFixture.library().tracks().reader(transaction).entryCount() == 0);
      CHECK_FALSE(libraryFixture.library().manifest().reader(transaction).get("image.flac"));
    }

    // A fresh command can import the current layout, never the prepared ranges.
    auto const retryRes = commandsFixture.runTask(commands.createTrackFromFileAsync(audio));
    REQUIRE(retryRes);
    REQUIRE(retryRes->trackIds.size() == 2);
    REQUIRE(publications.size() == 1);
    CHECK(publications.front().tracksInserted == retryRes->trackIds);
    CHECK(publications.front().libraryRevision == revisionBefore + 1);
    auto transaction = libraryFixture.library().readTransaction();
    auto const reader = libraryFixture.library().tracks().reader(transaction);
    auto const optFirst = reader.get(retryRes->trackIds[0]);
    auto const optSecond = reader.get(retryRes->trackIds[1]);
    REQUIRE(optFirst);
    REQUIRE(optSecond);
    REQUIRE(optFirst->property().segment());
    REQUIRE(optSecond->property().segment());
    CHECK(optFirst->property().segment()->startFrame == 0);
    CHECK(optFirst->property().segment()->frameCount == 29400);
    CHECK(optSecond->property().segment()->startFrame == 29400);
    CHECK(optSecond->property().segment()->frameCount == 58800);
    auto const optManifest = libraryFixture.library().manifest().reader(transaction).get("image.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->segmentCount() == 2);
    CHECK(optManifest->totalFrames() == 88200);
  }

  TEST_CASE("LibraryCommands - add-file rejects path changes during lane handoff", "[concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto& commands = commandsFixture.commands();
    auto const audio = copyFixtureAudio(libraryFixture, "song.flac");
    auto const revisionBefore = [&]
    {
      auto transaction = libraryFixture.library().readTransaction();
      return libraryFixture.library().libraryRevision(transaction);
    }();
    std::size_t publicationCount = 0;
    auto sub = changes.onChanged([&](LibraryChangeSet const&) noexcept { ++publicationCount; });

    SECTION("removed file")
    {
      auto const res = runImportAcrossAdmission(
        executor, commands.createTrackFromFileAsync(audio), [&] { REQUIRE(std::filesystem::remove(audio)); });
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("audio facts changed")
    {
      auto const res = runImportAcrossAdmission(executor,
                                                commands.createTrackFromFileAsync(audio),
                                                [&]
                                                {
                                                  auto output = std::ofstream{audio, std::ios::binary | std::ios::app};
                                                  output << 'x';
                                                  REQUIRE(output.good());
                                                });
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::Conflict);
      CHECK(res.error().message.contains("Track file changed"));
    }

    SECTION("input symlink retargeted inside the root")
    {
      auto const other = copyFixtureAudio(libraryFixture, "other.flac");
      auto const alias = libraryFixture.root() / "alias.flac";
      auto const symlink = ao::test::SymlinkFixture{audio, alias, ao::test::SymlinkType::File};
      auto const res = runImportAcrossAdmission(executor,
                                                commands.createTrackFromFileAsync(alias),
                                                [&]
                                                {
                                                  REQUIRE(std::filesystem::remove(alias));
                                                  std::filesystem::create_symlink(other, alias);
                                                });
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::Conflict);
      CHECK(res.error().message.contains("Track file path changed"));
    }

    SECTION("prepared path replaced with a symlink escaping the root")
    {
      auto const outside = ao::test::TempDir{};
      auto const outsideFile = outside.path() / "outside.flac";
      std::filesystem::copy_file(audio, outsideFile);
      auto const linkProbe = libraryFixture.root() / "probe.flac";
      auto const symlink = ao::test::SymlinkFixture{outsideFile, linkProbe, ao::test::SymlinkType::File};
      auto const res = runImportAcrossAdmission(executor,
                                                commands.createTrackFromFileAsync(audio),
                                                [&]
                                                {
                                                  REQUIRE(std::filesystem::remove(audio));
                                                  std::filesystem::create_symlink(outsideFile, audio);
                                                });
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("outside music root"));
    }

    CHECK(publicationCount == 0);
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == revisionBefore);
    CHECK(libraryFixture.library().tracks().reader(transaction).entryCount() == 0);
    auto const manifest = libraryFixture.library().manifest().reader(transaction);
    CHECK_FALSE(manifest.get("song.flac"));
    CHECK_FALSE(manifest.get("other.flac"));
    CHECK_FALSE(manifest.get("alias.flac"));
  }

  TEST_CASE("LibraryCommands - createTrackFromFile accepts root-relative paths")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    auto const absValidFile = copyFixtureAudio(libraryFixture, "relative.flac");
    REQUIRE(std::filesystem::is_regular_file(absValidFile));

    auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync("relative.flac"));
    REQUIRE(trackIdRes);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrackView = libraryFixture.library()
                                .tracks()
                                .reader(transaction)
                                .get(trackIdRes->trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrackView);
    CHECK(optTrackView->property().uri() == "relative.flac");
  }

  TEST_CASE("LibraryCommands - createTrackFromFile preserves a UTF-8 URI")
  {
    auto const expected = std::string{"\xE8\xAA\xB0\xE3\x81\x8B\xE3\x80\x81\xE6\xB5\xB7\xE3\x82\x92\xE3\x80\x82/"
                                      "Dvo\xC5\x99\xC3\xA1k.flac"};
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();
    auto const mediaPath = copyFixtureAudio(libraryFixture, utility::pathFromUtf8(expected));

    REQUIRE(std::filesystem::is_regular_file(mediaPath));

    auto const createdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(mediaPath));

    REQUIRE(createdRes);
    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrack = libraryFixture.library()
                            .tracks()
                            .reader(transaction)
                            .get(createdRes->trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == expected);
    CHECK(libraryFixture.library().manifest().reader(transaction).get(expected));
  }

  TEST_CASE("LibraryCommands - createTrackFromFile rejects unsupported files with Result errors")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const existingId = libraryFixture.addTrack("Existing");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();
    auto const revisionBefore = commandsFixture.bind(std::array{existingId}).revision();

    std::size_t publicationCount = 0;
    auto sub = changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    auto const unsupportedFile = createTextFile(libraryFixture, "unsupported.txt");
    REQUIRE(std::filesystem::is_regular_file(unsupportedFile));

    auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(unsupportedFile));
    REQUIRE_FALSE(trackIdRes);
    CHECK(trackIdRes.error().code == Error::Code::NotSupported);
    CHECK(publicationCount == 0);

    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == revisionBefore);
    auto const reader = libraryFixture.library().tracks().reader(transaction);
    CHECK(reader.entryCount() == 1);
    auto const optExisting = reader.get(existingId);
    REQUIRE(optExisting);
    CHECK(optExisting->metadata().title() == "Existing");
    CHECK_FALSE(libraryFixture.library().manifest().reader(transaction).get("unsupported.txt"));
  }

  TEST_CASE("LibraryCommands - createTrackFromFile rejects invalid path boundaries")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    SECTION("missing file")
    {
      auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync("missing.flac"));
      REQUIRE(!trackIdRes);
      CHECK(trackIdRes.error().code == Error::Code::NotFound);
    }

    SECTION("outside root")
    {
      auto const outsideTemp = ao::test::TempDir{};
      auto const outsideFile = outsideTemp.path() / "outside.flac";
      {
        auto out = std::ofstream{outsideFile};
        out << "not audio";
      }
      auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(outsideFile));
      REQUIRE(!trackIdRes);
      CHECK(trackIdRes.error().code == Error::Code::InvalidInput);
      CHECK(trackIdRes.error().message.contains("outside music root"));
    }

    SECTION("path too long for a library URI")
    {
      for (auto const componentCount : std::array<std::size_t, 2>{10, 21})
      {
        CAPTURE(componentCount);
        auto const longPath = longTrackPath(libraryFixture.root(), componentCount);
        auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(longPath));
        REQUIRE_FALSE(trackIdRes);
        CHECK(trackIdRes.error().code == Error::Code::ValueTooLarge);
        CHECK(trackIdRes.error().message.contains("track file path is too long"));
        CHECK(trackIdRes.error().message.contains("exceeds the maximum of 1976 bytes"));
      }
    }

#ifndef _WIN32
    // Windows forbids control characters in names; POSIX filesystems allow them.
    SECTION("name inside root that forms no URI")
    {
      auto const trackIdRes =
        commandsFixture.runTask(commands.createTrackFromFileAsync(libraryFixture.root() / "track\x01.flac"));
      REQUIRE_FALSE(trackIdRes);
      CHECK(trackIdRes.error().code == Error::Code::InvalidInput);
      CHECK(trackIdRes.error().message.contains("does not form a library URI"));
      CHECK(trackIdRes.error().message.contains("control characters"));
    }
#endif

    SECTION("long path outside root")
    {
      auto const outsideTemp = ao::test::TempDir{};

      for (auto const componentCount : std::array<std::size_t, 2>{10, 21})
      {
        CAPTURE(componentCount);
        auto const longPath = longTrackPath(outsideTemp.path(), componentCount);
        auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(longPath));
        REQUIRE_FALSE(trackIdRes);
        CHECK(trackIdRes.error().code == Error::Code::InvalidInput);
        CHECK(trackIdRes.error().message.contains("outside music root"));
      }
    }

    SECTION("symlink escaping root")
    {
      auto const outsideTemp = ao::test::TempDir{};
      auto const outsideFile = outsideTemp.path() / "outside.flac";
      {
        auto output = std::ofstream{outsideFile};
        output << "outside";
      }
      auto const alias = libraryFixture.root() / "alias.flac";
      auto const symlink = ao::test::SymlinkFixture{outsideFile, alias, ao::test::SymlinkType::File};

      auto const trackIdRes = commandsFixture.runTask(commands.createTrackFromFileAsync(alias));
      REQUIRE_FALSE(trackIdRes);
      CHECK(trackIdRes.error().code == Error::Code::InvalidInput);
      CHECK(trackIdRes.error().message.contains("outside music root"));
    }
  }

  TEST_CASE("LibraryCommands - createTrackFromFile measures a long path under a symlinked root")
  {
    auto const temp = ao::test::TempDir{};
    auto const realRoot = temp.path() / "real";
    std::filesystem::create_directories(realRoot);
    auto const configuredRoot = temp.path() / "configured";
    auto const symlink = ao::test::SymlinkFixture{realRoot, configuredRoot, ao::test::SymlinkType::Directory};
    auto library = library::test::makeTestMusicLibrary(configuredRoot, temp.path() / "db");
    auto changes = makeStateOnlyLibraryChanges(library);
    auto commandsFixture = LibraryCommandsFixture{library, changes};

    // The path is spelled through the configured root, so an unresolvable one
    // is measured relative to that spelling rather than the resolved root.
    for (auto const componentCount : std::array<std::size_t, 2>{10, 21})
    {
      CAPTURE(componentCount);
      auto const longPath = longTrackPath(configuredRoot, componentCount);
      auto const trackIdRes = commandsFixture.runTask(commandsFixture.commands().createTrackFromFileAsync(longPath));
      REQUIRE_FALSE(trackIdRes);
      CHECK(trackIdRes.error().code == Error::Code::ValueTooLarge);
      CHECK(trackIdRes.error().message.contains("exceeds the maximum of 1976 bytes"));
    }
  }

  TEST_CASE("LibraryCommands - createTrackFromFile reports a valid URI whose path the platform cannot resolve")
  {
    // A root of thirteen components and a URI of eight keep each within its
    // limit while the joined path exceeds Linux's 4,096 bytes.
    auto const temp = ao::test::TempDir{};
    auto root = temp.path();

    for (std::size_t index = 0; index < 13; ++index)
    {
      root /= std::string(200, 'r');
    }

    auto ec = std::error_code{};
    std::filesystem::create_directories(root, ec);

    if (ec)
    {
      SKIP("the host cannot create a path this long: " << ec.message());
    }

    auto const longPath = longTrackPath(root, 8);
    std::ignore = std::filesystem::weakly_canonical(longPath, ec);

    if (ec != std::errc::filename_too_long)
    {
      SKIP("the host resolves a path this long");
    }

    auto library = library::test::makeTestMusicLibrary(root, temp.path() / "db");
    auto changes = makeStateOnlyLibraryChanges(library);
    auto commandsFixture = LibraryCommandsFixture{library, changes};

    auto const trackIdRes = commandsFixture.runTask(commandsFixture.commands().createTrackFromFileAsync(longPath));
    REQUIRE_FALSE(trackIdRes);
    CHECK(trackIdRes.error().code == Error::Code::IoError);
    CHECK(trackIdRes.error().message.contains("too long for the platform"));
  }
} // namespace ao::rt::test
