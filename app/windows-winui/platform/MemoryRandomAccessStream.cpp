// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/MemoryRandomAccessStream.h"

#include <objidl.h>
#include <shcore.h>
#include <winrt/base.h>

#include <limits>

namespace ao::winui
{
  winrt::Windows::Storage::Streams::IRandomAccessStream makeMemoryRandomAccessStream(
    std::span<std::byte const> const bytes)
  {
    if (bytes.size() > std::numeric_limits<ULONG>::max())
    {
      throw winrt::hresult_invalid_argument{};
    }

    auto randomAccessStream = winrt::Windows::Storage::Streams::InMemoryRandomAccessStream{};
    auto stream = winrt::com_ptr<IStream>{};
    winrt::check_hresult(
      CreateStreamOverRandomAccessStream(winrt::get_unknown(randomAccessStream), __uuidof(IStream), stream.put_void()));

    auto offset = std::size_t{};
    while (offset < bytes.size())
    {
      auto written = ULONG{};
      winrt::check_hresult(stream->Write(bytes.data() + offset, static_cast<ULONG>(bytes.size() - offset), &written));
      if (written == 0)
      {
        throw winrt::hresult_error{STG_E_WRITEFAULT};
      }
      offset += written;
    }

    auto origin = LARGE_INTEGER{};
    winrt::check_hresult(stream->Seek(origin, STREAM_SEEK_SET, nullptr));
    randomAccessStream.Seek(0);
    return randomAccessStream;
  }
} // namespace ao::winui
