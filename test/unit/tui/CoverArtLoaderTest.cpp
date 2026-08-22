// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CoverArtLoader.h"

#include "CoverArtTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "tui/CoverArt.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/ResourceLayout.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    /// The bytes a cover request resolves to, standing in for the runtime walk.
    using ResourceByteMap = std::unordered_map<ResourceId, std::vector<std::byte>>;

    async::Task<Result<std::optional<std::vector<std::byte>>>> loadStoredResource(ResourceByteMap const* const source,
                                                                                  std::size_t* const readCount,
                                                                                  ResourceId const resourceId,
                                                                                  std::stop_token const stopToken)
    {
      async::throwIfStopRequested(stopToken);
      ++*readCount;
      auto const found = source->find(resourceId);

      if (found == source->end())
      {
        co_return std::optional<std::vector<std::byte>>{};
      }

      co_return std::optional{found->second};
    }

    struct CoverArtLoaderFixture final
    {
      CoverArtLoaderFixture()
      {
        _byteLoaderPtr = std::make_unique<rt::ResourceByteLoader>(
          _runtime, std::bind_front(loadStoredResource, &_bytesById, &_readCount));
      }

      ~CoverArtLoaderFixture()
      {
        _byteLoaderPtr.reset();
        _runtime.requestStop();
        _runtime.join();
      }

      CoverArtLoaderFixture(CoverArtLoaderFixture const&) = delete;
      CoverArtLoaderFixture& operator=(CoverArtLoaderFixture const&) = delete;
      CoverArtLoaderFixture(CoverArtLoaderFixture&&) = delete;
      CoverArtLoaderFixture& operator=(CoverArtLoaderFixture&&) = delete;

      /// The handle a real library would mint for this content, bound to bytes the
      /// fake delivers: cover delivery is what this suite tests, not the walk that
      /// produces the bytes.
      ResourceId addResource(std::span<std::byte const> bytes)
      {
        auto const resourceId = library::deriveResourceId(utility::computeSha256(bytes));
        _bytesById.insert_or_assign(resourceId, std::vector<std::byte>{bytes.begin(), bytes.end()});
        return resourceId;
      }

      rt::test::QueuedExecutor& executor() noexcept { return _executor; }
      async::Runtime& runtimeAsync() noexcept { return _runtime; }
      rt::ResourceByteLoader& byteLoader() const noexcept { return *_byteLoaderPtr; }
      rt::test::ControlledSleeper& sleeper() noexcept { return _sleeper; }
      /// How many resource reads the walk actually started.
      std::size_t readCount() const noexcept { return _readCount; }

      /**
       * @brief Lets the current selection's settle window elapse.
       *
       * A replaced request has its window cancelled, so the only window still
       * open belongs to whatever is selected now.
       */
      bool settleSelection() { return _sleeper.fireNext(); }

    private:
      ResourceByteMap _bytesById{};
      std::size_t _readCount = 0;
      rt::test::QueuedExecutor _executor{};
      rt::test::ControlledSleeper _sleeper{};
      async::Runtime _runtime{_executor, 1, &_sleeper};
      std::unique_ptr<rt::ResourceByteLoader> _byteLoaderPtr;
    };
  } // namespace

  TEST_CASE("CoverArtLoader - block delivery is asynchronous and idempotent", "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    bool completionOnExecutor = false;
    CoverArtLoader* observedLoader = nullptr;
    auto loader = CoverArtLoader{fixture.byteLoader(),
                                 fixture.runtimeAsync(),
                                 CoverArtDeliveryMode::Blocks,
                                 [&]
                                 {
                                   ++refreshCount;

                                   if (observedLoader != nullptr && observedLoader->preview())
                                   {
                                     completionOnExecutor = fixture.executor().isCurrent();
                                   }
                                 }};
    observedLoader = &loader;

    loader.request(resourceId);
    CHECK(refreshCount == 1);
    CHECK_FALSE(loader.preview());

    loader.request(resourceId);
    CHECK(refreshCount == 1);

    REQUIRE(fixture.settleSelection());
    REQUIRE(fixture.executor().drainUntil([&] { return loader.preview().has_value(); }));
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
      fixture.byteLoader(), fixture.runtimeAsync(), CoverArtDeliveryMode::Kitty, [&] { ++refreshCount; }};

    loader.request(resourceId);

    REQUIRE(fixture.settleSelection());
    REQUIRE(fixture.executor().drainUntil([&] { return loader.kittyPng().has_value(); }));
    CHECK(refreshCount == 2);
    REQUIRE(loader.kittyPng()->size() >= 24);
    CHECK(loader.kittyPng()->front() == std::byte{0x89});
  }

  TEST_CASE("CoverArtLoader - disabled delivery opens no window and reads nothing", "[tui][unit][cover-art]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loader =
      CoverArtLoader{fixture.byteLoader(), fixture.runtimeAsync(), CoverArtDeliveryMode::Off, [&] { ++refreshCount; }};

    loader.request(resourceId);
    fixture.executor().drain();

    // The selection is still tracked, so re-enabling delivery has an id to use,
    // but nothing was scheduled and nothing was read.
    CHECK(loader.resourceId() == resourceId);
    CHECK(refreshCount == 1);
    CHECK(fixture.sleeper().callCount() == 0);
    CHECK(fixture.readCount() == 0);
    CHECK_FALSE(loader.preview());
    CHECK_FALSE(loader.kittyPng());
  }

  TEST_CASE("CoverArtLoader - replacement prevents a stale cover from publishing",
            "[tui][regression][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const oldResourceId = fixture.addResource(support::onePixelRedPng());
    auto const missingResourceId = ResourceId{987654};
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{
      fixture.byteLoader(), fixture.runtimeAsync(), CoverArtDeliveryMode::Blocks, [&] { ++refreshCount; }};

    loader.request(oldResourceId);
    loader.request(missingResourceId);
    REQUIRE(refreshCount == 2);

    REQUIRE(fixture.settleSelection());
    REQUIRE(fixture.executor().drainUntil([&] { return refreshCount == 3; }));
    CHECK(loader.resourceId() == missingResourceId);
    CHECK_FALSE(loader.preview());
    CHECK_FALSE(loader.kittyPng());
    // The replaced selection never became a read at all.
    CHECK(fixture.readCount() == 1);
  }

  TEST_CASE("CoverArtLoader - a navigation burst publishes only what is still selected",
            "[tui][regression][cover-art][concurrency]")
  {
    // Detail follows the track table now, so holding an arrow key replaces the
    // requested cover many times before anything settles.
    constexpr std::size_t kBurstLength = 50;
    auto fixture = CoverArtLoaderFixture{};
    auto resourceIds = std::vector<ResourceId>{};
    resourceIds.reserve(kBurstLength);

    for (std::size_t index = 0; index < kBurstLength; ++index)
    {
      resourceIds.push_back(fixture.addResource(support::distinctPng(index)));
    }

    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{
      fixture.byteLoader(), fixture.runtimeAsync(), CoverArtDeliveryMode::Blocks, [&] { ++refreshCount; }};

    for (auto const resourceId : resourceIds)
    {
      loader.request(resourceId);
    }

    CHECK(refreshCount == kBurstLength);
    CHECK_FALSE(loader.preview());

    REQUIRE(fixture.settleSelection());
    REQUIRE(fixture.executor().drainUntil([&] { return loader.preview().has_value(); }));
    CHECK(loader.resourceId() == resourceIds.back());
    // One transform survives the burst; the rest were cancelled before they
    // could publish over the current selection.
    CHECK(refreshCount == kBurstLength + 1);

    fixture.executor().drain();
    CHECK(refreshCount == kBurstLength + 1);
    CHECK(loader.resourceId() == resourceIds.back());
    REQUIRE(loader.preview());
    CHECK(loader.preview()->size() == static_cast<std::size_t>(kCoverArtRows));
    // The point of the settle window: a fifty-step burst costs one read, not
    // fifty cover extractions the user never sees.
    CHECK(fixture.readCount() == 1);
  }

  TEST_CASE("CoverArtLoader - a settle window that expires after replacement reads nothing",
            "[tui][regression][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const firstResourceId = fixture.addResource(support::distinctPng(1));
    auto const secondResourceId = fixture.addResource(support::distinctPng(2));
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{
      fixture.byteLoader(), fixture.runtimeAsync(), CoverArtDeliveryMode::Blocks, [&] { ++refreshCount; }};

    loader.request(firstResourceId);
    // The window expires, but its resumption is still queued when the selection
    // moves on, so expiry and replacement race for the same loader.
    REQUIRE(fixture.settleSelection());
    loader.request(secondResourceId);
    REQUIRE(fixture.settleSelection());

    REQUIRE(fixture.executor().drainUntil([&] { return loader.preview().has_value(); }));
    CHECK(loader.resourceId() == secondResourceId);
    CHECK(fixture.readCount() == 1);
  }

  TEST_CASE("CoverArtLoader - repeated cancellation and clearing are idempotent", "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{
      fixture.byteLoader(), fixture.runtimeAsync(), CoverArtDeliveryMode::Blocks, [&] { ++refreshCount; }};

    loader.request(resourceId);
    REQUIRE(refreshCount == 1);

    loader.clear();
    loader.clear();
    loader.cancel();
    loader.cancel();
    fixture.executor().drain();

    CHECK(refreshCount == 2);
    CHECK(loader.resourceId() == kInvalidResourceId);
    CHECK_FALSE(loader.preview());
    CHECK(fixture.readCount() == 0);
  }

  TEST_CASE("CoverArtLoader - cancellation suppresses decode completion", "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loaderPtr = std::make_unique<CoverArtLoader>(
      fixture.byteLoader(), fixture.runtimeAsync(), CoverArtDeliveryMode::Blocks, [&] { ++refreshCount; });

    loaderPtr->request(resourceId);
    REQUIRE(refreshCount == 1);
    REQUIRE(fixture.sleeper().waitForCallCount(1));

    loaderPtr.reset();
    fixture.executor().drain();
    CHECK(refreshCount == 1);
    CHECK(fixture.readCount() == 0);
    CHECK(fixture.sleeper().waitForCancellation(0));
  }

  TEST_CASE("CoverArtLoader - destruction after the settle window suppresses decode completion",
            "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loaderPtr = std::make_unique<CoverArtLoader>(
      fixture.byteLoader(), fixture.runtimeAsync(), CoverArtDeliveryMode::Blocks, [&] { ++refreshCount; });

    loaderPtr->request(resourceId);
    REQUIRE(refreshCount == 1);
    REQUIRE(fixture.settleSelection());
    fixture.executor().checkQueued();

    loaderPtr.reset();
    fixture.executor().drain();
    CHECK(refreshCount == 1);
  }
} // namespace ao::tui::test
