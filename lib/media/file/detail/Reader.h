// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Content.h"
#include <ao/Error.h>
#include <ao/media/file/File.h>

#include <cstddef>
#include <span>

namespace ao::media::file::detail
{
  class Reader
  {
  public:
    explicit Reader(std::span<std::byte const> bytes)
      : _bytes{bytes}
    {
    }

    virtual ~Reader() = default;

    Reader(Reader const&) = delete;
    Reader& operator=(Reader const&) = delete;
    Reader(Reader&&) = delete;
    Reader& operator=(Reader&&) = delete;

    virtual Result<PayloadView> audioPayload() const = 0;
    virtual Result<Content> readContent() const = 0;

  protected:
    std::span<std::byte const> bytes() const noexcept { return _bytes; }

    PayloadView payloadRange(std::size_t offset, std::size_t length) const noexcept
    {
      return {.bytes = _bytes.subspan(offset, length), .offset = offset};
    }

  private:
    std::span<std::byte const> _bytes;
  };
} // namespace ao::media::file::detail
