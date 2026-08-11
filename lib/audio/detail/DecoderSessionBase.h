// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/SampleEncoding.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

namespace ao::audio::detail
{
  template<typename Derived>
  class DecoderSessionBase : public DecoderSession
  {
  public:
    // Construction preserves ordinary allocation failure. Once the session
    // exists, initialize() is the noexcept recoverable-error boundary.
    static Result<std::unique_ptr<Derived>> open(std::filesystem::path const& filePath,
                                                 std::optional<SampleEncoding> optOutputEncoding)
    {
      // make_unique cannot invoke Derived's private constructor from this friend
      // context. The owning pointer destroys every partial initialization path
      // before an error is returned.
      auto sessionPtr = std::unique_ptr<Derived>{new Derived{optOutputEncoding}};

      if (auto const result = sessionPtr->initialize(filePath); !result)
      {
        return std::unexpected{result.error()};
      }

      return std::move(sessionPtr);
    }

  private:
    friend Derived;
    DecoderSessionBase() = default;
  };
} // namespace ao::audio::detail
