// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// GCC TSan cannot model std::atomic_thread_fence. Parse Boost.Asio's internal
// fence wrapper with that diagnostic disabled, but parse <atomic> first so a
// later project-owned fence remains diagnosable. Async entry points include
// this prelude before Asio, limiting the workaround to translation units that
// actually consume Asio.
#if defined(__GNUC__) && !defined(__clang__) && defined(__SANITIZE_THREAD__)
#include <atomic>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtsan"
#include <boost/asio/detail/atomic_count.hpp>
#include <boost/asio/detail/fenced_block.hpp>
#pragma GCC diagnostic pop
#endif
