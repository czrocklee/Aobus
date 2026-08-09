// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackTestSupport.h"

#include "WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/CoverArt.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <string_view>

namespace ao::library::test
{
  namespace
  {
    TrackId addTrackImpl(LibraryWrite& write, TrackSpec const& spec)
    {
      auto builder = TrackBuilder::makeEmpty();
      applyTrackSpec(builder, spec);

      auto createRes = write.tracks().create(builder, FileManifestBuilder::makeEmpty());
      REQUIRE(createRes);
      return *createRes;
    }

    TrackSpec withUniqueFixtureUri(MusicLibrary& library, LibraryWrite& write, TrackSpec const& spec)
    {
      auto effectiveSpec = spec;
      auto writer = write.tracks();

      if (!writer.manifest(effectiveSpec.uri))
      {
        return effectiveSpec;
      }

      auto const sourcePath = library.rootPath() / std::filesystem::path{effectiveSpec.uri};
      auto const extension = sourcePath.extension().string();
      std::uint64_t sequence = 1;
      auto alias = std::filesystem::path{};

      while (true)
      {
        alias = std::filesystem::path{std::format(".aobus-test/track-{}{}", sequence, extension)};
        ++sequence;

        if (!writer.manifest(alias.generic_string()))
        {
          break;
        }
      }

      if (auto const aliasPath = library.rootPath() / alias; std::filesystem::is_regular_file(sourcePath))
      {
        std::filesystem::create_directories(aliasPath.parent_path());
        std::filesystem::copy_file(sourcePath, aliasPath, std::filesystem::copy_options::overwrite_existing);
      }

      effectiveSpec.uri = alias.generic_string();
      return effectiveSpec;
    }
  } // namespace

  TrackSpec makeTrackSpec(std::string_view title, std::uint16_t year)
  {
    return TrackSpec{.title = std::string{title}, .year = year};
  }

  TrackSpec makeEmptyTrackSpec(std::string_view uri)
  {
    return TrackSpec{.title = "",
                     .artist = "",
                     .album = "",
                     .uri = std::string{uri},
                     .year = 0,
                     .discNumber = 0,
                     .trackNumber = 0,
                     .duration = std::chrono::milliseconds{0},
                     .bitrate = Bitrate{},
                     .sampleRate = SampleRate{},
                     .channels = Channels{},
                     .bitDepth = BitDepth{}};
  }

  void applyTrackSpec(TrackBuilder& builder, TrackSpec const& spec)
  {
    builder.metadata()
      .title(spec.title)
      .artist(spec.artist)
      .album(spec.album)
      .albumArtist(spec.albumArtist)
      .genre(spec.genre)
      .composer(spec.composer)
      .conductor(spec.conductor)
      .ensemble(spec.ensemble)
      .work(spec.work)
      .movement(spec.movement)
      .soloist(spec.soloist)
      .year(spec.year)
      .discNumber(spec.discNumber)
      .discTotal(spec.discTotal)
      .trackNumber(spec.trackNumber)
      .trackTotal(spec.trackTotal)
      .movementNumber(spec.movementNumber)
      .movementTotal(spec.movementTotal);
    builder.property()
      .uri(spec.uri)
      .duration(spec.duration)
      .bitrate(spec.bitrate)
      .sampleRate(spec.sampleRate)
      .codec(spec.codec)
      .channels(spec.channels)
      .bitDepth(spec.bitDepth);

    builder.tags().clear();

    for (auto const& tag : spec.tags)
    {
      builder.tags().add(tag);
    }

    builder.customMetadata().clear();

    for (auto const& [key, value] : spec.customMetadata)
    {
      builder.customMetadata().add(key, value);
    }

    builder.coverArt().clear();

    if (spec.coverArtId != kInvalidResourceId)
    {
      builder.coverArt().add(PictureType::FrontCover, spec.coverArtId);
    }
  }

  TrackSpec trackSpecFromView(MusicLibrary const& library, TrackView const& view)
  {
    auto spec =
      TrackSpec{.title = std::string{view.metadata().title()},
                .artist = std::string{library.dictionary().getOrDefault(view.metadata().artistId())},
                .album = std::string{library.dictionary().getOrDefault(view.metadata().albumId())},
                .albumArtist = std::string{library.dictionary().getOrDefault(view.metadata().albumArtistId())},
                .genre = std::string{library.dictionary().getOrDefault(view.metadata().genreId())},
                .composer = std::string{library.dictionary().getOrDefault(view.metadata().composerId())},
                .conductor = std::string{library.dictionary().getOrDefault(view.classical().conductorId())},
                .ensemble = std::string{library.dictionary().getOrDefault(view.classical().ensembleId())},
                .work = std::string{library.dictionary().getOrDefault(view.classical().workId())},
                .movement = std::string{library.dictionary().getOrDefault(view.classical().movementId())},
                .soloist = std::string{library.dictionary().getOrDefault(view.classical().soloistId())},
                .uri = std::string{view.property().uri()},
                .year = view.metadata().year(),
                .discNumber = view.metadata().discNumber(),
                .discTotal = view.metadata().discTotal(),
                .trackNumber = view.metadata().trackNumber(),
                .trackTotal = view.metadata().trackTotal(),
                .movementNumber = view.classical().movementNumber(),
                .movementTotal = view.classical().movementTotal(),
                .duration = view.property().duration(),
                .bitrate = view.property().bitrate(),
                .sampleRate = view.property().sampleRate(),
                .channels = view.property().channels(),
                .bitDepth = view.property().bitDepth(),
                .codec = view.property().codec()};

    for (auto const tagId : view.tags())
    {
      spec.tags.emplace_back(library.dictionary().getOrDefault(tagId));
    }

    for (auto const [keyId, value] : view.customMetadata())
    {
      spec.customMetadata.emplace_back(std::string{library.dictionary().getOrDefault(keyId)}, std::string{value});
    }

    if (auto const optCover = view.coverArt().primary(); optCover)
    {
      spec.coverArtId = optCover->resourceId;
    }

    return spec;
  }

  TrackId addTrack([[maybe_unused]] MusicLibrary& library, LibraryWrite& write, TrackSpec const& spec)
  {
    return addTrackImpl(write, spec);
  }

  TrackId addTrack(MusicLibrary& library, TrackSpec const& spec)
  {
    auto transaction = writeTransaction(library);
    auto idRes = transaction.apply([&library, &spec](LibraryWrite& write) -> Result<TrackId>
                                   { return addTrack(library, write, spec); });
    REQUIRE(idRes);
    REQUIRE(transaction.commit());
    return *idRes;
  }

  TrackId addTrackWithUniqueFixtureUri(MusicLibrary& library, LibraryWrite& write, TrackSpec const& spec)
  {
    auto const effectiveSpec = withUniqueFixtureUri(library, write, spec);
    return addTrackImpl(write, effectiveSpec);
  }

  TrackId addTrackWithUniqueFixtureUri(MusicLibrary& library, TrackSpec const& spec)
  {
    auto transaction = writeTransaction(library);
    auto idRes = transaction.apply([&library, &spec](LibraryWrite& write) -> Result<TrackId>
                                   { return addTrackWithUniqueFixtureUri(library, write, spec); });
    REQUIRE(idRes);
    REQUIRE(transaction.commit());
    return *idRes;
  }

  void mutateTrack(MusicLibrary& library, TrackId id, std::move_only_function<void(TrackBuilder&)> mutate)
  {
    auto transaction = writeTransaction(library);
    auto mutationRes = transaction.apply(
      [&library, id, &mutate](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto optView = writer.get(id, TrackStore::Reader::LoadMode::Both);
        REQUIRE(optView);

        auto const oldUri = std::string{optView->property().uri()};
        auto const optManifest = writer.manifest(oldUri);
        REQUIRE(optManifest);
        auto builder = TrackBuilder::fromCompleteView(*optView, library.dictionary());
        mutate(builder);

        if (builder.property().uri() == oldUri)
        {
          return writer.update(id, builder);
        }

        return writer.relink(id, builder, FileManifestBuilder::fromView(*optManifest));
      });
    REQUIRE(mutationRes);

    REQUIRE(transaction.commit());
  }

  void updateTrackSpec(MusicLibrary& library, TrackId id, std::move_only_function<void(TrackSpec&)> updater)
  {
    auto spec = TrackSpec{};
    {
      auto transaction = library.readTransaction();
      auto reader = library.tracks().reader(transaction);
      auto optView = reader.get(id, TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      spec = trackSpecFromView(library, *optView);
    }

    updater(spec);
    mutateTrack(library, id, [&](TrackBuilder& builder) { applyTrackSpec(builder, spec); });
  }
} // namespace ao::library::test
