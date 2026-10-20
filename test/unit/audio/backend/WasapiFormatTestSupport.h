// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// mmreg.h assumes the base Win32 types and pointer macros from windows.h.
// clang-format off
#include <windows.h>
#include <mmreg.h>
// clang-format on

namespace ao::audio::backend::test
{
  // Independently authored two-channel IEEE-float engine descriptor with a
  // complete FL/FR mask; callers mutate it for their own evidence cases.
  WAVEFORMATEXTENSIBLE extensibleFloatStereoWave();
} // namespace ao::audio::backend::test
