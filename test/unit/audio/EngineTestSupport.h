// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "ScriptedDecoderSession.h"
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  inline Device makeEngineTestDevice(std::string_view id = "test-device")
  {
    return {.id = DeviceId{std::string{id}},
            .displayName = "Test",
            .description = "Test",
            .isDefault = false,
            .backendId = kBackendNone};
  }

  inline Format makeEngineTestFormat()
  {
    return {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
  }

  inline auto makeScriptedEngineDecoderFactory(Format fmt = makeEngineTestFormat())
  {
    return [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});
      auto data = std::vector(100, std::byte{0});

      decPtr->setReadScript({{data, false}, {{}, true}});
      return decPtr;
    };
  }
} // namespace ao::audio::test
