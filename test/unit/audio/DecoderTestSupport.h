// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::audio
{
  class DecoderSession;
}

namespace ao::audio::test
{
  struct TerminalReadResult final
  {
    std::uint64_t frames = 0;
    std::optional<Error> optError;
  };

  void checkClosedSession(DecoderSession& decoder);
  std::uint64_t readUntilStableEndOfStream(DecoderSession& decoder, std::size_t maxBlocks);
  TerminalReadResult readUntilTerminalState(DecoderSession& decoder, std::size_t maxBlocks);
} // namespace ao::audio::test
