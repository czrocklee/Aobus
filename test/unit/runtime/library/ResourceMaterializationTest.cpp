// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ResourceMaterialization.h"

#include "runtime/library/ResourceCarrierIndex.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/media/file/TestFile.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/resource/ResourceDiskCache.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::filesystem::path copyCoverFixture(std::filesystem::path const& root, std::string_view const name)
    {
      auto const target = root / name;
      std::filesystem::copy_file(std::filesystem::path{AUDIO_TEST_DATA_DIR} / "with_cover.flac", target);
      return target;
    }

    /// A library whose tracks reference one cover held by real media files.
    struct CarrierFixture final
    {
      explicit CarrierFixture(std::size_t const carrierCount)
      {
        for (std::size_t index = 0; index < carrierCount; ++index)
        {
          uris.push_back("carrier" + std::to_string(index) + ".flac");
          copyCoverFixture(temp.path(), uris.back());
        }

        pictureBytes = media::file::test::requireSoleEmbeddedPicture(temp.path() / uris.front());
        digest = utility::computeSha256(pictureBytes);

        auto transaction = library::test::writeTransaction(library);
        auto resourceIdRes = library::test::physicalWriter(library.resources(), transaction).create(pictureBytes);
        REQUIRE(resourceIdRes);
        resourceId = *resourceIdRes;

        REQUIRE(transaction.apply(
          [this](library::LibraryWrite& write) -> Result<>
          {
            auto trackWriter = write.tracks();

            for (auto const& uri : uris)
            {
              auto builder = library::TrackBuilder::makeEmpty();
              builder.property().uri(uri);
              builder.coverArt().add(PictureType::FrontCover, resourceId);
              auto manifest = library::FileManifestBuilder::makeEmpty();
              manifest.status(library::FileStatus::Available);

              if (auto createRes = trackWriter.create(builder, manifest); !createRes)
              {
                return std::unexpected{createRes.error()};
              }
            }

            return {};
          }));
        REQUIRE(transaction.commit());
      }

      library::ResourceDescriptor descriptor() const
      {
        return library::ResourceDescriptor{
          .digest = digest, .byteLength = static_cast<std::uint32_t>(pictureBytes.size())};
      }

      ResourceCarrierIndex buildIndex() const
      {
        auto const transaction = library.readTransaction();
        return buildResourceCarrierIndex(library, transaction);
      }

      ao::test::TempDir temp{};
      library::MusicLibrary library{library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db")};
      std::vector<std::string> uris{};
      std::vector<std::byte> pictureBytes{};
      utility::Sha256Digest digest{};
      ResourceId resourceId = kInvalidResourceId;
    };

    ResourceDiskCache makeCache(std::filesystem::path const& root)
    {
      return ResourceDiskCache{ResourceDiskCache::Config{
        .directory = coverCacheDirectory(root),
        .maximumEntryBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
      }};
    }

    ResourceDiskCache makeDisabledCache()
    {
      return ResourceDiskCache{ResourceDiskCache::Config{}};
    }
  } // namespace

  TEST_CASE("materializeResource - a carrier that still holds the content answers", "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{1};
    auto const index = fixture.buildIndex();
    auto const cache = makeDisabledCache();
    auto const context = ResourceMaterializationContext{
      .descriptor = fixture.descriptor(),
      .candidateUris = index.carrierUris(fixture.resourceId),
      .musicRoot = fixture.library.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    auto result = materializeResource(context, {});
    REQUIRE(result);
    REQUIRE(*result);
    CHECK(**result == fixture.pictureBytes);
  }

  TEST_CASE("materializeResource - a track whose own file is gone is served from another carrier",
            "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{3};
    std::filesystem::remove(fixture.temp.path() / fixture.uris[0]);
    std::filesystem::remove(fixture.temp.path() / fixture.uris[1]);

    auto const index = fixture.buildIndex();
    auto const cache = makeDisabledCache();
    auto const context = ResourceMaterializationContext{
      .descriptor = fixture.descriptor(),
      .candidateUris = index.carrierUris(fixture.resourceId),
      .musicRoot = fixture.library.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    // A failed source is never a failed request: a missing file costs a failed
    // open and the walk advances.
    auto result = materializeResource(context, {});
    REQUIRE(result);
    REQUIRE(*result);
    CHECK(**result == fixture.pictureBytes);
  }

  TEST_CASE("materializeResource - a carrier that carries no matching picture advances to the next",
            "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{2};

    // A file that parses but no longer holds the content is a failed candidate,
    // not a failed request. Rescanning is what reconciles the stale reference.
    std::filesystem::remove(fixture.temp.path() / fixture.uris[0]);
    std::filesystem::copy_file(
      std::filesystem::path{AUDIO_TEST_DATA_DIR} / "basic_metadata.flac", fixture.temp.path() / fixture.uris[0]);

    auto const index = fixture.buildIndex();
    auto const cache = makeDisabledCache();
    auto const context = ResourceMaterializationContext{
      .descriptor = fixture.descriptor(),
      .candidateUris = index.carrierUris(fixture.resourceId),
      .musicRoot = fixture.library.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    auto result = materializeResource(context, {});
    REQUIRE(result);
    REQUIRE(*result);
    CHECK(**result == fixture.pictureBytes);
  }

  TEST_CASE("materializeResource - no carrier and no cache entry is no image", "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{1};
    std::filesystem::remove(fixture.temp.path() / fixture.uris[0]);

    auto const index = fixture.buildIndex();
    auto const cache = makeDisabledCache();
    auto const context = ResourceMaterializationContext{
      .descriptor = fixture.descriptor(),
      .candidateUris = index.carrierUris(fixture.resourceId),
      .musicRoot = fixture.library.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    auto result = materializeResource(context, {});
    REQUIRE(result);
    CHECK_FALSE(*result);
  }

  TEST_CASE("materializeResource - a carrier hit is cached, and the next request opens no file",
            "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{1};
    auto const index = fixture.buildIndex();
    auto const cache = makeCache(fixture.temp.path() / "cache");
    auto const context = ResourceMaterializationContext{
      .descriptor = fixture.descriptor(),
      .candidateUris = index.carrierUris(fixture.resourceId),
      .musicRoot = fixture.library.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    REQUIRE(materializeResource(context, {}));
    REQUIRE(cache.read(fixture.digest));

    // Every carrier is gone, so a hit now proves the cache answered alone: a valid
    // entry keeps a cover displayable until it is evicted.
    std::filesystem::remove(fixture.temp.path() / fixture.uris[0]);
    auto result = materializeResource(context, {});
    REQUIRE(result);
    REQUIRE(*result);
    CHECK(**result == fixture.pictureBytes);
  }

  TEST_CASE("materializeResource - a stale reference no file can satisfy is not rewritten",
            "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{1};
    std::filesystem::remove(fixture.temp.path() / fixture.uris[0]);
    std::filesystem::copy_file(
      std::filesystem::path{AUDIO_TEST_DATA_DIR} / "basic_metadata.flac", fixture.temp.path() / fixture.uris[0]);

    auto const index = fixture.buildIndex();
    auto const cache = makeDisabledCache();
    auto const context = ResourceMaterializationContext{
      .descriptor = fixture.descriptor(),
      .candidateUris = index.carrierUris(fixture.resourceId),
      .musicRoot = fixture.library.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    auto result = materializeResource(context, {});
    REQUIRE(result);
    CHECK_FALSE(*result);

    // A degradation state never removes or rewrites a track's cover reference.
    auto const transaction = fixture.library.readTransaction();
    auto const reader = fixture.library.tracks().reader(transaction);
    std::size_t count = 0;

    for (auto const& [trackId, view] : reader.cold())
    {
      ++count;
      REQUIRE(view.coverArt().count() == 1);
      CHECK(view.coverArt().at(0).resourceId == fixture.resourceId);
    }

    CHECK(count == 1);
    CHECK(fixture.library.resources().reader(transaction).get(fixture.resourceId));
  }

  TEST_CASE("materializeResource - materialized bytes above the caller's ceiling are refused",
            "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{1};
    auto const index = fixture.buildIndex();
    auto const cache = makeDisabledCache();

    SECTION("an interactive ceiling refuses them")
    {
      auto const context = ResourceMaterializationContext{
        .descriptor = fixture.descriptor(),
        .candidateUris = index.carrierUris(fixture.resourceId),
        .musicRoot = fixture.library.rootPath(),
        .cache = cache,
        .optMaximumBytes = fixture.pictureBytes.size() - 1,
      };

      auto result = materializeResource(context, {});
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("an administrative request has none")
    {
      auto const context = ResourceMaterializationContext{
        .descriptor = fixture.descriptor(),
        .candidateUris = index.carrierUris(fixture.resourceId),
        .musicRoot = fixture.library.rootPath(),
        .cache = cache,
        .optMaximumBytes = std::nullopt,
      };

      auto result = materializeResource(context, {});
      REQUIRE(result);
      REQUIRE(*result);
      CHECK((*result)->size() == fixture.pictureBytes.size());
    }
  }

  TEST_CASE("materializeResource - a declared length far from the truth blocks nothing",
            "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{1};
    auto const index = fixture.buildIndex();
    auto const cache = makeDisabledCache();

    // A figure nothing verified must never decide whether content is read: a
    // read cannot repair a row, so a wrong hint would hide the picture forever.
    auto descriptor = fixture.descriptor();
    descriptor.byteLength = 40U * 1024U * 1024U;
    auto const context = ResourceMaterializationContext{
      .descriptor = descriptor,
      .candidateUris = index.carrierUris(fixture.resourceId),
      .musicRoot = fixture.library.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    auto result = materializeResource(context, {});
    REQUIRE(result);
    REQUIRE(*result);
    CHECK((*result)->size() == fixture.pictureBytes.size());
  }

  TEST_CASE("materializeResource - cancellation stops the walk between candidates",
            "[runtime][unit][resource-walk][concurrency]")
  {
    auto fixture = CarrierFixture{3};
    auto const index = fixture.buildIndex();
    auto const cache = makeDisabledCache();
    auto const context = ResourceMaterializationContext{
      .descriptor = fixture.descriptor(),
      .candidateUris = index.carrierUris(fixture.resourceId),
      .musicRoot = fixture.library.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    auto stopSource = std::stop_source{};
    REQUIRE(stopSource.request_stop());
    CHECK_THROWS_AS(std::ignore = materializeResource(context, stopSource.get_token()), async::OperationCancelled);
  }

  TEST_CASE("ResourceCarrierIndex - a snapshot answers every request no newer than its stamp",
            "[runtime][unit][resource-walk]")
  {
    auto const snapshot = ResourceCarrierIndex{7, {}};

    // A request reads its revision from a transaction that pins one and then
    // loads the snapshot slot, which does not, so it can hold a snapshot built
    // for a later revision. That snapshot answers it, and not because it is a
    // superset — a later revision can have dropped references — but because a
    // candidate list is evidence: every candidate is accepted only on its
    // digest, so a dropped one ends in a miss rather than in wrong bytes. Asking
    // for equality instead would send a request that already holds a usable
    // snapshot to wait on the rebuild mutex behind an unrelated build.
    CHECK(snapshot.answersRevision(6));
    CHECK(snapshot.answersRevision(7));
    CHECK_FALSE(snapshot.answersRevision(8));

    // The rebuild re-checks the slot under its lock with this same question, so
    // the two sites cannot drift into accepting different snapshots.
    CHECK(ResourceCarrierIndex{}.answersRevision(0));
    CHECK_FALSE(ResourceCarrierIndex{}.answersRevision(1));
  }

  TEST_CASE("buildResourceCarrierIndex - names every referencing file and nothing else",
            "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{3};

    // A file carrying the same content but referenced by nothing is not a
    // candidate: discovery reaches only the persisted reference graph.
    copyCoverFixture(fixture.temp.path(), "unreferenced.flac");

    auto const index = fixture.buildIndex();
    CHECK(index.resourceCount() == 1);
    auto const candidates = index.carrierUris(fixture.resourceId);
    REQUIRE(candidates.size() == 3);

    for (auto const& uri : candidates)
    {
      CHECK(uri != "unreferenced.flac");
    }

    CHECK(index.carrierUris(ResourceId{999999}).empty());

    SECTION("candidate order is stable across rebuilds")
    {
      auto const rebuilt = fixture.buildIndex();
      auto const rebuiltCandidates = rebuilt.carrierUris(fixture.resourceId);
      REQUIRE(rebuiltCandidates.size() == candidates.size());

      for (std::size_t position = 0; position < candidates.size(); ++position)
      {
        CHECK(rebuiltCandidates[position] == candidates[position]);
      }
    }

    SECTION("the snapshot is stamped with the revision it was built from")
    {
      auto const transaction = fixture.library.readTransaction();
      CHECK(index.libraryRevision() == fixture.library.libraryRevision(transaction));
    }
  }

  TEST_CASE("materializeResource - a restored library serves a cover with no rescan",
            "[runtime][unit][resource-walk][cover]")
  {
    auto source = CarrierFixture{1};
    auto const yamlPath = std::filesystem::path{source.temp.path()} / "library.yaml";
    REQUIRE(LibraryYamlExporter{source.library}.exportToYaml(yamlPath, ExportMode::Full));

    // The restore lands beside the music it describes, which is what makes the
    // reference graph enough: nothing re-extracted, nothing rescanned.
    auto restored =
      library::test::makeTestMusicLibrary(source.temp.path(), std::filesystem::path{source.temp.path()} / "restored");
    REQUIRE(LibraryYamlImporter{restored}.importFromYamlOffline(yamlPath, ImportMode::Restore));

    auto const resourceId = library::deriveResourceId(source.digest);
    auto optIndex = std::optional<ResourceCarrierIndex>{};
    auto optDescriptor = std::optional<library::ResourceDescriptor>{};
    {
      auto const transaction = restored.readTransaction();
      optIndex.emplace(buildResourceCarrierIndex(restored, transaction));
      optDescriptor = restored.resources().reader(transaction).get(resourceId);
    }

    REQUIRE(optDescriptor);
    CHECK(optDescriptor->digest == source.digest);

    auto const cache = makeDisabledCache();
    auto const context = ResourceMaterializationContext{
      .descriptor = *optDescriptor,
      .candidateUris = optIndex->carrierUris(resourceId),
      .musicRoot = restored.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    auto result = materializeResource(context, {});
    REQUIRE(result);
    REQUIRE(*result);
    CHECK(**result == source.pictureBytes);
  }

  TEST_CASE("materializeResource - a resource with no candidate at all is no image", "[runtime][unit][resource-walk]")
  {
    auto fixture = CarrierFixture{1};
    auto const index = fixture.buildIndex();
    auto const cache = makeDisabledCache();
    auto const context = ResourceMaterializationContext{
      .descriptor = fixture.descriptor(),
      .candidateUris = index.carrierUris(ResourceId{424242}),
      .musicRoot = fixture.library.rootPath(),
      .cache = cache,
      .optMaximumBytes = LibraryJobs::kMaximumInteractiveResourceBytes,
    };

    auto result = materializeResource(context, {});
    REQUIRE(result);
    CHECK_FALSE(*result);
  }
} // namespace ao::rt::test
