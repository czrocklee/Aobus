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
#include <ao/rt/resource/ResourceByteMemoryCache.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

    struct ResourceReadLog final
    {
      mutable std::mutex mutex;
      std::vector<ResourceId> ids;
    };

    async::Task<Result<std::optional<std::vector<std::byte>>>> readStoredResourceAsync(
      ResourceByteMap const* const source,
      ResourceReadLog* const readLog,
      ResourceId const resourceId,
      std::stop_token const stopToken)
    {
      async::throwIfStopRequested(stopToken);
      {
        auto const lock = std::scoped_lock{readLog->mutex};
        readLog->ids.push_back(resourceId);
      }

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
        _byteCachePtr = std::make_unique<rt::ResourceByteMemoryCache>(
          _runtime, std::bind_front(readStoredResourceAsync, &_bytesById, &_readLog));
      }

      ~CoverArtLoaderFixture()
      {
        _byteCachePtr.reset();
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
      rt::ResourceByteMemoryCache& byteCache() const noexcept { return *_byteCachePtr; }
      rt::test::ControlledSleeper& sleeper() noexcept { return _sleeper; }
      /// How many resource reads the walk actually started.
      std::size_t readCount() const
      {
        auto const lock = std::scoped_lock{_readLog.mutex};
        return _readLog.ids.size();
      }

      std::vector<ResourceId> readResourceIds() const
      {
        auto const lock = std::scoped_lock{_readLog.mutex};
        return _readLog.ids;
      }

      /**
       * @brief Lets the current selection's settle window elapse.
       *
       * A replaced request has its window cancelled, so the only window still
       * open belongs to whatever is selected now.
       */
      bool trySettleSelection() { return _sleeper.tryFireNext(); }

    private:
      ResourceByteMap _bytesById{};
      ResourceReadLog _readLog;
      rt::test::QueuedExecutor _executor{};
      rt::test::ControlledSleeper _sleeper{};
      async::Runtime _runtime{_executor, 1, &_sleeper};
      std::unique_ptr<rt::ResourceByteMemoryCache> _byteCachePtr;
    };
  } // namespace

  TEST_CASE("CoverArtLoader - block delivery is asynchronous and idempotent", "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    bool completionOnExecutor = false;
    CoverArtLoader* observedLoader = nullptr;
    auto loader = CoverArtLoader{fixture.byteCache(),
                                 fixture.runtimeAsync(),
                                 CoverArtDeliveryMode::Blocks,
                                 [&]
                                 {
                                   ++refreshCount;

                                   if (observedLoader != nullptr && observedLoader->preview())
                                   {
                                     completionOnExecutor = fixture.executor().isCurrent();
                                   }
                                 },
                                 kCoverArtDefaultColumns};
    observedLoader = &loader;

    loader.request(resourceId);
    CHECK(refreshCount == 1);
    CHECK_FALSE(loader.preview());

    loader.request(resourceId);
    CHECK(refreshCount == 1);

    REQUIRE(fixture.trySettleSelection());
    REQUIRE(fixture.executor().tryDrainUntil([&] { return loader.preview().has_value(); }));
    CHECK(refreshCount == 2);
    CHECK(completionOnExecutor);
    CHECK(loader.resourceId() == resourceId);
    REQUIRE(loader.preview()->size() == static_cast<std::size_t>(kCoverArtRows));
    REQUIRE(loader.preview()->front().size() == static_cast<std::size_t>(loader.columns()));
  }

  TEST_CASE("CoverArtLoader - block delivery decodes at the width it was given", "[tui][unit][cover-art]")
  {
    // The width Detail reserves is measured from the terminal, so a loader that
    // decoded against its own idea of the slot would publish artwork the pane
    // cannot hold. Anything but the constructed width fails here.
    constexpr std::int32_t kRequestedColumns = kCoverArtDefaultColumns + 7;

    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    auto loader = CoverArtLoader{
      fixture.byteCache(), fixture.runtimeAsync(), CoverArtDeliveryMode::Blocks, [] {}, kRequestedColumns};

    CHECK(loader.columns() == kRequestedColumns);

    loader.request(resourceId);

    REQUIRE(fixture.trySettleSelection());
    REQUIRE(fixture.executor().tryDrainUntil([&] { return loader.preview().has_value(); }));
    REQUIRE(loader.preview()->size() == static_cast<std::size_t>(kCoverArtRows));
    CHECK(loader.preview()->front().size() == static_cast<std::size_t>(kRequestedColumns));
  }

  TEST_CASE("CoverArtLoader - a negative measured width clamps to zero", "[tui][unit][cover-art]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto loader = CoverArtLoader{fixture.byteCache(), fixture.runtimeAsync(), CoverArtDeliveryMode::Blocks, [] {}, -1};

    CHECK(loader.columns() == 0);
  }

  TEST_CASE("CoverArtLoader - Kitty delivery publishes bounded PNG output", "[tui][unit][cover-art]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{fixture.byteCache(),
                                 fixture.runtimeAsync(),
                                 CoverArtDeliveryMode::Kitty,
                                 [&] { ++refreshCount; },
                                 kCoverArtDefaultColumns};

    loader.request(resourceId);

    REQUIRE(fixture.trySettleSelection());
    REQUIRE(fixture.executor().tryDrainUntil([&] { return loader.kittyPng().has_value(); }));
    CHECK(refreshCount == 2);
    REQUIRE(loader.kittyPng()->size() >= 24);
    CHECK(loader.kittyPng()->size() <= kMaximumGeneratedCoverArtBytes);
    CHECK(loader.kittyPng()->front() == std::byte{0x89});
  }

  TEST_CASE("CoverArtLoader - disabled delivery opens no window and reads nothing", "[tui][unit][cover-art]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{fixture.byteCache(),
                                 fixture.runtimeAsync(),
                                 CoverArtDeliveryMode::Off,
                                 [&] { ++refreshCount; },
                                 kCoverArtDefaultColumns};

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
            "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const oldResourceId = fixture.addResource(support::onePixelRedPng());
    auto const missingResourceId = ResourceId{987654};
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{fixture.byteCache(),
                                 fixture.runtimeAsync(),
                                 CoverArtDeliveryMode::Blocks,
                                 [&] { ++refreshCount; },
                                 kCoverArtDefaultColumns};

    loader.request(oldResourceId);
    loader.request(missingResourceId);
    REQUIRE(refreshCount == 2);

    REQUIRE(fixture.trySettleSelection());
    REQUIRE(fixture.executor().tryDrainUntil([&] { return refreshCount == 3; }));
    CHECK(loader.resourceId() == missingResourceId);
    CHECK_FALSE(loader.preview());
    CHECK_FALSE(loader.kittyPng());
    // The replaced selection never became a read at all.
    CHECK(fixture.readResourceIds() == std::vector{missingResourceId});
  }

  TEST_CASE("CoverArtLoader - a navigation burst publishes only what is still selected",
            "[tui][unit][cover-art][concurrency]")
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
    auto loader = CoverArtLoader{fixture.byteCache(),
                                 fixture.runtimeAsync(),
                                 CoverArtDeliveryMode::Blocks,
                                 [&] { ++refreshCount; },
                                 kCoverArtDefaultColumns};

    for (auto const resourceId : resourceIds)
    {
      loader.request(resourceId);
    }

    CHECK(refreshCount == kBurstLength);
    CHECK_FALSE(loader.preview());

    REQUIRE(fixture.trySettleSelection());
    REQUIRE(fixture.executor().tryDrainUntil([&] { return loader.preview().has_value(); }));
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
    CHECK(fixture.readResourceIds() == std::vector{resourceIds.back()});
  }

  TEST_CASE("CoverArtLoader - an expired stale settle window reads nothing after replacement",
            "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const firstResourceId = fixture.addResource(support::distinctPng(1));
    auto const secondResourceId = fixture.addResource(support::distinctPng(2));
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{fixture.byteCache(),
                                 fixture.runtimeAsync(),
                                 CoverArtDeliveryMode::Blocks,
                                 [&] { ++refreshCount; },
                                 kCoverArtDefaultColumns};

    loader.request(firstResourceId);
    // The window expires, but its resumption is still queued when the selection
    // moves on, so expiry and replacement race for the same loader.
    REQUIRE(fixture.trySettleSelection());
    loader.request(secondResourceId);
    REQUIRE(fixture.trySettleSelection());

    REQUIRE(fixture.executor().tryDrainUntil([&] { return loader.preview().has_value(); }));
    CHECK(loader.resourceId() == secondResourceId);
    CHECK(fixture.readResourceIds() == std::vector{secondResourceId});
  }

  TEST_CASE("CoverArtLoader - repeated cancellation and clearing are idempotent", "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{fixture.byteCache(),
                                 fixture.runtimeAsync(),
                                 CoverArtDeliveryMode::Blocks,
                                 [&] { ++refreshCount; },
                                 kCoverArtDefaultColumns};

    loader.request(resourceId);
    REQUIRE(refreshCount == 1);

    REQUIRE(fixture.sleeper().tryWaitForCallCount(1));
    loader.cancel();
    loader.cancel();
    REQUIRE(fixture.sleeper().tryWaitForCancellation(0));
    fixture.executor().drain();
    CHECK(refreshCount == 1);
    CHECK(loader.resourceId() == resourceId);
    CHECK_FALSE(loader.preview());
    CHECK(fixture.readCount() == 0);

    loader.clear();
    loader.clear();
    CHECK(refreshCount == 2);
    CHECK(loader.resourceId() == kInvalidResourceId);
    CHECK_FALSE(loader.preview());
    CHECK(fixture.readCount() == 0);
  }

  TEST_CASE("CoverArtLoader - live mode change cancels pending delivery and can resume",
            "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    auto loader = CoverArtLoader{
      fixture.byteCache(), fixture.runtimeAsync(), CoverArtDeliveryMode::Blocks, [] {}, kCoverArtDefaultColumns};
    loader.request(resourceId);
    REQUIRE(fixture.sleeper().tryWaitForCallCount(1));
    loader.setMode(CoverArtDeliveryMode::Off);
    fixture.executor().drain();
    CHECK_FALSE(loader.preview());
    CHECK_FALSE(loader.kittyPng());
    CHECK(fixture.readCount() == 0);
    CHECK(fixture.sleeper().tryWaitForCancellation(0));
    loader.setMode(CoverArtDeliveryMode::Blocks);
    loader.request(resourceId);
    REQUIRE(fixture.trySettleSelection());
    REQUIRE(fixture.executor().tryDrainUntil([&] { return loader.preview().has_value(); }));
    CHECK(loader.resourceId() == resourceId);
    CHECK(fixture.readResourceIds() == std::vector{resourceId});
  }

  TEST_CASE("CoverArtLoader - cached cover republishes after a missing selection", "[tui][unit][cover-art]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    auto const missingId = ResourceId{987654};
    std::size_t refreshCount = 0;
    auto loader = CoverArtLoader{fixture.byteCache(),
                                 fixture.runtimeAsync(),
                                 CoverArtDeliveryMode::Blocks,
                                 [&] { ++refreshCount; },
                                 kCoverArtDefaultColumns};
    loader.request(resourceId);
    REQUIRE(fixture.trySettleSelection());
    REQUIRE(fixture.executor().tryDrainUntil([&] { return loader.preview().has_value(); }));
    REQUIRE_FALSE(loader.preview()->empty());
    REQUIRE_FALSE(loader.preview()->front().empty());
    CHECK(loader.preview()->front().front().topRed > 200);
    CHECK(fixture.readResourceIds() == std::vector{resourceId});

    loader.request(missingId);
    REQUIRE(fixture.trySettleSelection());
    REQUIRE(fixture.executor().tryDrainUntil([&] { return refreshCount == 4; }));
    CHECK(loader.resourceId() == missingId);
    CHECK_FALSE(loader.preview());
    CHECK(fixture.readResourceIds() == std::vector{resourceId, missingId});

    loader.request(resourceId);
    REQUIRE(fixture.trySettleSelection());
    REQUIRE(fixture.executor().tryDrainUntil([&] { return loader.preview().has_value(); }));
    CHECK(loader.resourceId() == resourceId);
    CHECK(refreshCount == 6);
    REQUIRE_FALSE(loader.preview()->empty());
    REQUIRE_FALSE(loader.preview()->front().empty());
    auto const& pixel = loader.preview()->front().front();
    CHECK(pixel.topRed > 200);
    CHECK(pixel.topGreen < 50);
    CHECK(pixel.topBlue < 50);
    CHECK(fixture.readResourceIds() == std::vector{resourceId, missingId});
  }

  TEST_CASE("CoverArtLoader - destruction during settling cancels the window without reading or refreshing",
            "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loaderPtr = std::make_unique<CoverArtLoader>(
      fixture.byteCache(),
      fixture.runtimeAsync(),
      CoverArtDeliveryMode::Blocks,
      [&] { ++refreshCount; },
      kCoverArtDefaultColumns);

    loaderPtr->request(resourceId);
    REQUIRE(refreshCount == 1);
    REQUIRE(fixture.sleeper().tryWaitForCallCount(1));

    loaderPtr.reset();
    fixture.executor().drain();
    CHECK(refreshCount == 1);
    CHECK(fixture.readCount() == 0);
    CHECK(fixture.sleeper().tryWaitForCancellation(0));
  }

  TEST_CASE("CoverArtLoader - destruction suppresses a queued settle resumption", "[tui][unit][cover-art][concurrency]")
  {
    auto fixture = CoverArtLoaderFixture{};
    auto const resourceId = fixture.addResource(support::onePixelRedPng());
    std::size_t refreshCount = 0;
    auto loaderPtr = std::make_unique<CoverArtLoader>(
      fixture.byteCache(),
      fixture.runtimeAsync(),
      CoverArtDeliveryMode::Blocks,
      [&] { ++refreshCount; },
      kCoverArtDefaultColumns);

    loaderPtr->request(resourceId);
    REQUIRE(refreshCount == 1);
    REQUIRE(fixture.trySettleSelection());
    fixture.executor().checkQueued();

    loaderPtr.reset();
    fixture.executor().drain();
    CHECK(refreshCount == 1);
    CHECK(fixture.readCount() == 0);
  }
} // namespace ao::tui::test
