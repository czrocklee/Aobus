// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "FrameLayout.h"
#include <cassert>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string_view>
#include <vector>

namespace rs::tag::mpeg
{
  class FrameView
  {
  public:
    FrameView(void const* data, std::size_t /*size*/) : _data{data} {}

    std::size_t length() const;
    bool isValid() const;

    FrameLayout const& layout() const { return *static_cast<FrameLayout const*>(_data); }
    void const* data() const { return _data; }

    std::uint32_t sampleRate() const;
    std::uint32_t bitrate() const;
    std::uint8_t channels() const;
    std::uint16_t samplesPerFrame() const;

    struct XingInfo
    {
      std::uint32_t frames = 0;
      std::uint32_t bytes = 0;
    };
    std::optional<XingInfo> xingInfo() const;

  private:
    void const* _data = nullptr;
  };

  std::optional<FrameView> locate(void const* buffer, std::size_t size);
}
