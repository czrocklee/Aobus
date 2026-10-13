// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PerformanceAllocation.h"
#include "PerformanceReport.h"
#include <ao/Error.h>

#include <catch2/catch_config.hpp>
#include <catch2/catch_session.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <print>
#include <utility>

#ifdef _WIN32
#include <malloc.h>
#else
#include <limits>
#endif

namespace
{
  struct AllocationCounter final
  {
    std::mutex mutex;
    std::atomic<bool> active{false};
    ao::rt::test::AllocationSample sample;
  };

  AllocationCounter& allocationCounter()
  {
    // Constant initialization cannot recursively allocate while new is entering.
    static constinit auto counter = AllocationCounter{};
    return counter;
  }

  void recordAllocation(std::size_t const bytes)
  {
    auto& counter = allocationCounter();

    if (!counter.active.load(std::memory_order_relaxed))
    {
      return;
    }

    auto lock = std::scoped_lock{counter.mutex};

    if (counter.active.load(std::memory_order_relaxed))
    {
      ++counter.sample.calls;
      counter.sample.requestedBytes += bytes;
    }
  }

  void* allocate(std::size_t const requested, std::size_t const alignment = 0)
  {
    auto const bytes = requested == 0 ? std::size_t{1} : requested;

    for (;;)
    {
      void* memory = nullptr;

      if (alignment == 0)
      {
        // Replacement new must use the C allocation backend, not call itself.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
        memory = std::malloc(bytes);
      }
      else
      {
#ifdef _WIN32
        memory = ::_aligned_malloc(bytes, alignment);
#else
        if (bytes <= std::numeric_limits<std::size_t>::max() - (alignment - 1))
        {
          // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) -- C backend for aligned replacement new.
          memory = std::aligned_alloc(alignment, ((bytes + alignment - 1) / alignment) * alignment);
        }
#endif
      }

      if (memory != nullptr)
      {
        recordAllocation(requested);
        return memory;
      }

      auto const handler = std::get_new_handler();

      if (handler == nullptr)
      {
        throw std::bad_alloc{};
      }

      handler();
    }
  }

  void freeUnaligned(void* memory) noexcept
  {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc) -- Matched C allocation backend.
    std::free(memory);
  }

  void freeAligned(void* memory) noexcept
  {
#ifdef _WIN32
    ::_aligned_free(memory);
#else
    freeUnaligned(memory);
#endif
  }
} // namespace

// These replacements belong only to ao_perf_baseline, never the shared test
// support library. Sized deletes and every aligned/array/nothrow path share
// the same allocation family; no pointer registry or inferred heap accounting.
// Standard declarations supply inherited nodiscard and vendor-private parameter names.
// Neither belongs to the portable replacement implementation's naming contract.
// NOLINTBEGIN(aobus-modernize-nodiscard-usage,readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t bytes)
{
  return allocate(bytes);
}
void* operator new[](std::size_t bytes)
{
  return allocate(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment)
{
  return allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment)
{
  return allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t bytes, std::nothrow_t const& /*tag*/) noexcept
{
  try
  {
    return allocate(bytes);
  }
  catch (...)
  {
    return nullptr;
  }
}
void* operator new[](std::size_t bytes, std::nothrow_t const& /*tag*/) noexcept
{
  try
  {
    return allocate(bytes);
  }
  catch (...)
  {
    return nullptr;
  }
}
void* operator new(std::size_t bytes, std::align_val_t alignment, std::nothrow_t const& /*tag*/) noexcept
{
  try
  {
    return allocate(bytes, static_cast<std::size_t>(alignment));
  }
  catch (...)
  {
    return nullptr;
  }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, std::nothrow_t const& /*tag*/) noexcept
{
  try
  {
    return allocate(bytes, static_cast<std::size_t>(alignment));
  }
  catch (...)
  {
    return nullptr;
  }
}
void operator delete(void* memory) noexcept
{
  freeUnaligned(memory);
}
void operator delete[](void* memory) noexcept
{
  freeUnaligned(memory);
}
void operator delete(void* memory, std::size_t /*bytes*/) noexcept
{
  freeUnaligned(memory);
}
void operator delete[](void* memory, std::size_t /*bytes*/) noexcept
{
  freeUnaligned(memory);
}
void operator delete(void* memory, std::nothrow_t const& /*tag*/) noexcept
{
  freeUnaligned(memory);
}
void operator delete[](void* memory, std::nothrow_t const& /*tag*/) noexcept
{
  freeUnaligned(memory);
}
void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept
{
  freeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept
{
  freeAligned(memory);
}
void operator delete(void* memory, std::size_t /*bytes*/, std::align_val_t /*alignment*/) noexcept
{
  freeAligned(memory);
}
void operator delete[](void* memory, std::size_t /*bytes*/, std::align_val_t /*alignment*/) noexcept
{
  freeAligned(memory);
}
void operator delete(void* memory, std::align_val_t /*alignment*/, std::nothrow_t const& /*tag*/) noexcept
{
  freeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t /*alignment*/, std::nothrow_t const& /*tag*/) noexcept
{
  freeAligned(memory);
}

// NOLINTEND(aobus-modernize-nodiscard-usage,readability-inconsistent-declaration-parameter-name)

namespace ao::rt::test
{
  AllocationSampleScope::AllocationSampleScope()
  {
    auto& counter = allocationCounter();
    auto lock = std::scoped_lock{counter.mutex};

    if (counter.active.load(std::memory_order_relaxed))
    {
      std::abort();
    }

    counter.sample = {};
    counter.active.store(true, std::memory_order_relaxed);
  }

  AllocationSampleScope::~AllocationSampleScope()
  {
    if (_active)
    {
      std::ignore = finish();
    }
  }

  AllocationSample AllocationSampleScope::finish()
  {
    auto& counter = allocationCounter();
    auto lock = std::scoped_lock{counter.mutex};

    if (!_active)
    {
      std::abort();
    }

    counter.active.store(false, std::memory_order_relaxed);
    _active = false;
    return counter.sample;
  }
} // namespace ao::rt::test

int main(int argc, char* argv[])
{
  auto session = Catch::Session{};
  auto const status = session.run(argc, argv);
  // Informational commands have no workload to publish. Failed workloads may
  // leave diagnostic v2 output, but must not publish a successful v1 artifact.
  if (auto const& config = session.configData(); status != 0 || config.showHelp || config.libIdentify ||
                                                 config.listTests || config.listTags || config.listReporters ||
                                                 config.listListeners)
  {
    return status;
  }

  if (auto reportRes = ao::rt::test::writeRequestedBaselineReport(); !reportRes)
  {
    std::println(std::cerr, "Aobus performance baseline: {}", reportRes.error().message);
    return 1;
  }

  return 0;
}
