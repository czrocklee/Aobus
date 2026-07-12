// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>

#include <expected>
#include <filesystem>

namespace ao::audio::detail
{
  template<typename Derived>
  class DecoderSessionBase : public DecoderSession
  {
  public:
    // DecoderSession deliberately fail-fast terminates if allocation or another
    // programming failure escapes this recoverable-error boundary.
    Result<> open(std::filesystem::path const& filePath) noexcept override
    {
      auto* derived = static_cast<Derived*>(this);
      derived->close();

      if (auto const result = derived->openCodec(filePath); !result)
      {
        derived->close();
        return std::unexpected{result.error()};
      }

      return {};
    }

  private:
    friend Derived;
    DecoderSessionBase() = default;
  };
} // namespace ao::audio::detail
