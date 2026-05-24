// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AudioBackendBootstrap.h"

#include <ao/rt/AppRuntime.h>

#include <memory>

#ifdef ALSA_FOUND
#include "ao/audio/backend/AlsaProvider.h"
#endif
#ifdef PIPEWIRE_FOUND
#include "ao/audio/backend/PipeWireProvider.h"
#endif

namespace ao::gtk
{
  void registerPlatformAudioBackends(rt::AppRuntime& runtime)
  {
#ifdef PIPEWIRE_FOUND
    runtime.addAudioProvider(std::make_unique<audio::backend::PipeWireProvider>());
#endif
#ifdef ALSA_FOUND
    runtime.addAudioProvider(std::make_unique<audio::backend::AlsaProvider>());
#endif
  }
} // namespace ao::gtk
