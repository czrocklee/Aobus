// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/MemoryRandomAccessStream.h>

#include <objidl.h>
#include <shcore.h>
#include <windows.h>
#include <winrt/base.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>

namespace ao::winui
{
  PreparedMemoryRandomAccessStream::~PreparedMemoryRandomAccessStream()
  {
    reset();
  }

  PreparedMemoryRandomAccessStream::PreparedMemoryRandomAccessStream(PreparedMemoryRandomAccessStream&& other) noexcept
    : _handle{other.release()}
  {
  }

  PreparedMemoryRandomAccessStream& PreparedMemoryRandomAccessStream::operator=(
    PreparedMemoryRandomAccessStream&& other) noexcept
  {
    if (this != &other)
    {
      reset();
      _handle = other.release();
    }

    return *this;
  }

  void* PreparedMemoryRandomAccessStream::release() noexcept
  {
    return std::exchange(_handle, nullptr);
  }

  void PreparedMemoryRandomAccessStream::reset() noexcept
  {
    if (_handle != nullptr)
    {
      std::ignore = ::GlobalFree(_handle);
      _handle = nullptr;
    }
  }

  PreparedMemoryRandomAccessStream prepareMemoryRandomAccessStream(std::span<std::byte const> const bytes)
  {
    if (bytes.empty())
    {
      return {};
    }

    auto* const handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes.size());

    if (handle == nullptr)
    {
      winrt::throw_last_error();
    }

    auto prepared = PreparedMemoryRandomAccessStream{handle};
    auto* const target = ::GlobalLock(handle);

    if (target == nullptr)
    {
      winrt::throw_last_error();
    }

    std::memcpy(target, bytes.data(), bytes.size());
    std::ignore = ::GlobalUnlock(handle);
    return prepared;
  }

  winrt::Windows::Storage::Streams::IRandomAccessStream makeMemoryRandomAccessStream(
    PreparedMemoryRandomAccessStream prepared)
  {
    auto stream = winrt::com_ptr<IStream>{};
    winrt::check_hresult(::CreateStreamOnHGlobal(prepared._handle, TRUE, stream.put()));
    std::ignore = prepared.release();
    auto randomAccessStream = winrt::Windows::Storage::Streams::IRandomAccessStream{nullptr};
    winrt::check_hresult(
      ::CreateRandomAccessStreamOverStream(stream.get(),
                                           BSOS_DEFAULT,
                                           winrt::guid_of<winrt::Windows::Storage::Streams::IRandomAccessStream>(),
                                           winrt::put_abi(randomAccessStream)));
    return randomAccessStream;
  }
} // namespace ao::winui
