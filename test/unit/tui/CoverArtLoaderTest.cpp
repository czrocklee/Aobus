// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CoverArtLoader.h"

#include "CoverArtTestSupport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    struct CoverArtLoaderFixture final
    {
      rt::test::MusicLibraryFixture storage{};
      rt::test::QueuedExecutor executor{};
      async::Runtime runtime{executor, 1};
      rt::LibraryChanges changes{};
      std::unique_ptr<rt::Library> libraryPtr;

      CoverArtLoaderFixture() = default;
      ~CoverArtLoaderFixture()
      {
        runtime.requestStop();
        runtime.join();
      }

      CoverArtLoaderFixture(CoverArtLoaderFixture const&) = delete;
      CoverArtLoaderFixture& operator=(CoverArtLoaderFixture const&) = delete;
      CoverArtLoaderFixture(CoverArtLoaderFixture&&) = delete;
      CoverArtLoaderFixture& operator=(CoverArtLoaderFixture&&) = delete;

      ResourceId addResource(std::span<std::byte const> bytes)
      {
        auto transaction = library::test::writeTransaction(storage.library());
        auto result = storage.library().resources().writer(transaction).create(bytes);
        REQUIRE(result);
        REQUIRE(transaction.commit());
        return *result;
      }

      rt::Library& library()
      {
        if (!libraryPtr)
        {
          libraryPtr = std::make_unique<rt::Library>(runtime, storage.library(), changes);
        }

        return *libraryPtr;
      }
    };
  } // namespace

  TEST_CASE("CoverArtLoader - block delivery is asynchronous and idempotent", "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    bool completionOnExecutor = false;
    CoverArtLoader* observedLoader = nullptr;
    auto loader = CoverArtLoader{fixture.library().taskService(),
                                 fixture.runtime,
                                 CoverArtDeliveryMode::Blocks,
                                 [&]
                                 {
                                   ++refreshCount;

                                   if (observedLoader != nullptr && observedLoader->preview())
                                   {
                                     completionOnExecutor = fixture.executor.isCurrent();
                                   }
                                 }};
    observedLoader = &loader;

    loader.request(resourceId);
    CHECK(refreshCount == 1);
    CHECK_FALSE(loader.preview());

    loader.request(resourceId);
    CHECK(refreshCount == 1);

    REQUIRE(fixture.executor.drainUntil([&] { return loader.preview().has_value(); }));
    CHECK(refreshCount == 2);
    CHECK(completionOnExecutor);
    CHECK(loader.resourceId() == resourceId);
    REQUIRE(loader.preview()->size() == 12);
    REQUIRE(loader.preview()->front().size() == 24);
  }

  TEST_CASE("CoverArtLoader - Kitty delivery publishes bounded PNG output", "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{
      fixture.library().taskService(), fixture.runtime, CoverArtDeliveryMode::Kitty, [&] { ++refreshCount; }};

    loader.request(resourceId);

    REQUIRE(fixture.executor.drainUntil([&] { return loader.kittyPng().has_value(); }));
    CHECK(refreshCount == 2);
    REQUIRE(loader.kittyPng()->size() >= 24);
    CHECK(loader.kittyPng()->front() == std::byte{0x89});
  }

  TEST_CASE("CoverArtLoader - replacement prevents a stale cover from publishing",
            "[tui][regression][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const oldResourceId = fixture.addResource(support::onePixelRedPng());
    auto const missingResourceId = ResourceId{987654};
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{
      fixture.library().taskService(), fixture.runtime, CoverArtDeliveryMode::Blocks, [&] { ++refreshCount; }};

    loader.request(oldResourceId);
    loader.request(missingResourceId);
    REQUIRE(refreshCount == 2);

    REQUIRE(fixture.executor.drainUntil([&] { return refreshCount == 3; }));
    CHECK(loader.resourceId() == missingResourceId);
    CHECK_FALSE(loader.preview());
    CHECK_FALSE(loader.kittyPng());
  }

  TEST_CASE("CoverArtLoader - cancellation suppresses decode completion", "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loaderPtr = std::make_unique<CoverArtLoader>(
      fixture.library().taskService(), fixture.runtime, CoverArtDeliveryMode::Blocks, [&] { ++refreshCount; });

    loaderPtr->request(resourceId);
    REQUIRE(refreshCount == 1);
    fixture.executor.checkQueued();

    loaderPtr.reset();
    fixture.executor.drain();
    CHECK(refreshCount == 1);
  }
} // namespace ao::tui::test
