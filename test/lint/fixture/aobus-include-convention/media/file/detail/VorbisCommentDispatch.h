// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

struct VorbisCommentDispatchEntry
{
  char const* name;
  Handler handler;
};

inline constexpr VorbisCommentDispatchEntry kGeneratedDispatchEntries[] = {
  {.name = "TITLE", .handler = &handleGenerated},
};
