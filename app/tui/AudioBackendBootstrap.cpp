// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AudioBackendBootstrap.h"

#include <ao/audio/BackendProvider.h>
#include <ao/rt/AppRuntime.h>

#include <utility>

namespace ao::tui
{
  void registerPlatformAudioBackends(rt::AppRuntime& runtime)
  {
    for (auto& providerPtr : audio::createPlatformBackendProviders())
    {
      runtime.addAudioProvider(std::move(providerPtr));
    }
  }
} // namespace ao::tui
