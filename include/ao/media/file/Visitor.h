// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/PictureType.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ao::media::file
{
  enum class TextField : std::uint8_t
  {
    Title,
    Artist,
    Album,
    AlbumArtist,
    Composer,
    Genre,
    Work,
    Movement,
  };

  enum class NumberField : std::uint8_t
  {
    Year,
    TrackNumber,
    TrackTotal,
    DiscNumber,
    DiscTotal,
    MovementNumber,
    MovementTotal,
  };

  enum class CreditKind : std::uint8_t
  {
    Conductor,
    Ensemble,
    Soloist,
    Performer,
  };

  // Source credit; trimmed, nonblank name and an optional descriptive role.
  struct CreditView final
  {
    std::string_view name{};
    CreditKind kind = CreditKind::Performer;
    std::string_view role{};
  };

  class Visitor
  {
  public:
    Visitor() = default;
    virtual ~Visitor() = default;

    Visitor(Visitor const&) = delete;
    Visitor& operator=(Visitor const&) = delete;
    Visitor(Visitor&&) = delete;
    Visitor& operator=(Visitor&&) = delete;

    virtual void text(TextField field, std::string_view value) = 0;
    virtual void number(NumberField field, std::uint16_t value) = 0;
    virtual void codec(AudioCodec value) = 0;
    virtual void duration(std::chrono::milliseconds duration) = 0;
    virtual void bitrate(Bitrate value) = 0;
    virtual void sampleRate(SampleRate value) = 0;
    virtual void channels(Channels value) = 0;
    virtual void bitDepth(BitDepth value) = 0;
    virtual void picture(PictureType type, std::span<std::byte const> bytes) = 0;

    /**
     * Ordered whole-list credits, emitted synchronously at most once per visited
     * content, only when the list is nonempty, after every other callback.
     * The span and its strings borrow the reader's storage for the lifetime
     * of the visited content; the default implementation ignores them.
     */
    virtual void visitCredits(std::span<CreditView const> /*credits*/) {}
  };
} // namespace ao::media::file
