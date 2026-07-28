// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/Windows.Storage.Streams.h>

#include <span>

namespace ao::winui
{
  class PreparedMemoryRandomAccessStream final
  {
  public:
    PreparedMemoryRandomAccessStream() = default;
    ~PreparedMemoryRandomAccessStream();

    PreparedMemoryRandomAccessStream(PreparedMemoryRandomAccessStream const&) = delete;
    PreparedMemoryRandomAccessStream& operator=(PreparedMemoryRandomAccessStream const&) = delete;
    PreparedMemoryRandomAccessStream(PreparedMemoryRandomAccessStream&& other) noexcept;
    PreparedMemoryRandomAccessStream& operator=(PreparedMemoryRandomAccessStream&& other) noexcept;
    explicit operator bool() const noexcept { return _handle != nullptr; }

  private:
    explicit PreparedMemoryRandomAccessStream(void* handle) noexcept
      : _handle{handle}
    {
    }

    void* release() noexcept;
    void reset() noexcept;

    void* _handle = nullptr;

    friend PreparedMemoryRandomAccessStream prepareMemoryRandomAccessStream(std::span<std::byte const> bytes);
    friend winrt::Windows::Storage::Streams::IRandomAccessStream makeMemoryRandomAccessStream(
      PreparedMemoryRandomAccessStream prepared);
  };

  PreparedMemoryRandomAccessStream prepareMemoryRandomAccessStream(std::span<std::byte const> bytes);
  winrt::Windows::Storage::Streams::IRandomAccessStream makeMemoryRandomAccessStream(
    PreparedMemoryRandomAccessStream prepared);
} // namespace ao::winui
