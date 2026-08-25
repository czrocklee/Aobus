// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/CoreAudioProvider.h"

#include <ao/audio/BackendProvider.h>

#include <memory>
#include <vector>

namespace ao::audio
{
  std::vector<std::unique_ptr<BackendProvider>> createPlatformBackendProviders()
  {
    auto providers = std::vector<std::unique_ptr<BackendProvider>>{};
    providers.push_back(std::make_unique<backend::CoreAudioProvider>());
    return providers;
  }
} // namespace ao::audio
