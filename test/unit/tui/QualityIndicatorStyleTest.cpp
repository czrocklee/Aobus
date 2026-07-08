// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/QualityIndicatorStyle.h"

#include <ao/audio/Quality.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::tui::test
{
  TEST_CASE("QualityIndicatorStyle - uses Soul brand quality colors", "[tui][unit][quality]")
  {
    auto style = qualityIndicatorStyle(audio::Quality::BitwisePerfect);
    CHECK(style.red == 0xA8);
    CHECK(style.green == 0x55);
    CHECK(style.blue == 0xF7);
    CHECK(style.label == "Bit-perfect playback");

    style = qualityIndicatorStyle(audio::Quality::LosslessFloat);
    CHECK(style.red == 0x10);
    CHECK(style.green == 0xB9);
    CHECK(style.blue == 0x81);
    CHECK(style.label == "Signal preserved");

    style = qualityIndicatorStyle(audio::Quality::LinearIntervention);
    CHECK(style.red == 0xF5);
    CHECK(style.green == 0x9E);
    CHECK(style.blue == 0x0B);

    style = qualityIndicatorStyle(uimodel::AudioQualityCategory::Warning);
    CHECK(style.red == 0xF5);
    CHECK(style.green == 0x9E);
    CHECK(style.blue == 0x0B);

    style = qualityIndicatorStyle(audio::Quality::LossySource);
    CHECK(style.red == 0x6B);
    CHECK(style.green == 0x72);
    CHECK(style.blue == 0x80);

    style = qualityIndicatorStyle(audio::Quality::Clipped);
    CHECK(style.red == 0xEF);
    CHECK(style.green == 0x44);
    CHECK(style.blue == 0x44);

    style = qualityIndicatorStyle(audio::Quality::Unknown);
    CHECK(style.red == 0x6B);
    CHECK(style.green == 0x72);
    CHECK(style.blue == 0x80);
    CHECK(style.label == "Unknown quality");
  }
} // namespace ao::tui::test
