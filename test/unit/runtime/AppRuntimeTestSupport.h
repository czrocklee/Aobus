// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendProvider.h>
#include <ao/rt/AppRuntime.h>

#include <memory>

namespace ao::async
{
  class Executor;
  class Sleeper;
}

namespace ao::test
{
  class TempDir;
}

namespace ao::rt
{
  class ConfigStore;
  class PlaybackTransport;
}

namespace ao::rt::test
{
  audio::BackendProvider::Status makeReadyAudioStatus();
  audio::BackendProvider::Status makePipeWireOutputStatus();
  std::unique_ptr<audio::BackendProvider> makeReadyAudioProvider();
  std::unique_ptr<audio::BackendProvider> makeReadyAudioProvider(audio::BackendProvider::Status status);

  void addReadyAudioProvider(PlaybackTransport& transport);
  void addReadyAudioProvider(PlaybackTransport& transport, audio::BackendProvider::Status status);
  void addReadyAudioProvider(AppRuntime& runtime);
  void addReadyAudioProvider(AppRuntime& runtime, audio::BackendProvider::Status status);

  std::unique_ptr<AppRuntime> makeRuntime(ao::test::TempDir const& tempDir,
                                          std::unique_ptr<async::Executor> executorPtr,
                                          ConfigStore* playbackSessionConfigStore = nullptr,
                                          async::Sleeper* sleeper = nullptr);

  std::unique_ptr<AppRuntime> makeRuntime(ao::test::TempDir const& tempDir,
                                          ConfigStore* playbackSessionConfigStore = nullptr,
                                          async::Sleeper* sleeper = nullptr);
} // namespace ao::rt::test
