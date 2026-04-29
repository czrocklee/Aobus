// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/IMainThreadDispatcher.h"
#include "core/backend/NullBackend.h"
#include "core/playback/PlaybackEngine.h"
#include <catch2/catch.hpp>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace app::core::playback;
using namespace app::core::backend;
using namespace app::core;

namespace
{
  class ImmediateDispatcher : public IMainThreadDispatcher
  {
  public:
    void dispatch(std::function<void()> callback) override { callback(); }
  };
}

TEST_CASE("PlaybackEngine - Graph Integrity", "[playback][integration][graph]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";
  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found, skipping Graph Integrity test");
    return;
  }

  auto dispatcher = std::make_shared<ImmediateDispatcher>();
  auto backend = std::make_unique<NullBackend>();
  AudioDevice device{
    .id = "null", .displayName = "Null", .description = "Null", .isDefault = false, .backendKind = BackendKind::None};

  PlaybackEngine engine(std::move(backend), device, dispatcher);

  TrackPlaybackDescriptor descriptor;
  descriptor.filePath = testFile.string();
  descriptor.title = "Test Title";
  descriptor.artist = "Test Artist";

  engine.play(descriptor);

  // Wait for the engine to open the track and populate the graph
  // We need to wait because it happens on a background thread
  bool decoderFound = false;
  bool engineFound = false;

  for (int i = 0; i < 50; ++i)
  {
    auto snap = engine.snapshot();
    decoderFound = false;
    engineFound = false;

    for (auto const& node : snap.graph.nodes)
    {
      if (node.id == "rs-decoder") decoderFound = true;
      if (node.id == "rs-engine") engineFound = true;
    }

    if (decoderFound && engineFound) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  SECTION("rs-decoder is present in the graph")
  {
    CHECK(decoderFound);
  }

  SECTION("rs-engine is present in the graph")
  {
    CHECK(engineFound);
  }

  SECTION("rs-decoder has valid format")
  {
    auto snap = engine.snapshot();
    auto it = std::ranges::find_if(snap.graph.nodes, [](auto const& n) { return n.id == "rs-decoder"; });
    if (it != snap.graph.nodes.end())
    {
      REQUIRE(it->format.has_value());
      CHECK(it->format->sampleRate == 44100);
      CHECK(it->format->channels == 2);
      CHECK(it->format->bitDepth == 16);
    }
  }

  engine.stop();
}
