// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/WasapiProvider.h"
#include <ao/audio/BackendProvider.h>

#include <memory>
#include <vector>

namespace ao::audio
{
  std::vector<std::unique_ptr<BackendProvider>> createPlatformBackendProviders()
  {
    auto providers = std::vector<std::unique_ptr<BackendProvider>>{};
    providers.push_back(std::make_unique<backend::WasapiProvider>());
    return providers;
  }
} // namespace ao::audio
