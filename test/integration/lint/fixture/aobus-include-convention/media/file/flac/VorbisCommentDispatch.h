// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

struct FlacVorbisDispatchEntry
{
  char const* name;
  Handler handler;
};

inline constexpr FlacVorbisDispatchEntry kGeneratedDispatchEntries[] = {
  {.name = "TITLE", .handler = &handleGenerated},
};
