// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AudioBackendBootstrap.h"

#include <ao/audio/BackendConfig.h>
#include <ao/rt/AppRuntime.h>

#include <memory>

#if AOBUS_HAS_ALSA
#include <ao/audio/backend/AlsaProvider.h>
#endif
#if AOBUS_HAS_PIPEWIRE
#include <ao/audio/backend/PipeWireProvider.h>
#endif

namespace ao::gtk
{
  void registerPlatformAudioBackends(rt::AppRuntime& runtime)
  {
#if AOBUS_HAS_PIPEWIRE
    runtime.addAudioProvider(std::make_unique<audio::backend::PipeWireProvider>());
#endif
#if AOBUS_HAS_ALSA
    runtime.addAudioProvider(std::make_unique<audio::backend::AlsaProvider>());
#endif
  }
} // namespace ao::gtk
