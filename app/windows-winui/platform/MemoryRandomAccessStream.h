// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/Windows.Storage.Streams.h>

#include <cstddef>
#include <span>

namespace ao::winui
{
  winrt::Windows::Storage::Streams::IRandomAccessStream makeMemoryRandomAccessStream(std::span<std::byte const> bytes);
} // namespace ao::winui
