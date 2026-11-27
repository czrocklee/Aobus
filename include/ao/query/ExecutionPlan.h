// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Opaque handle for a compiled query.
//
// ExecutionPlan is produced by query::compileQuery() and consumed by
// query::PlanEvaluator. Code that only stores or passes a compiled plan around
// (e.g. holding a std::unique_ptr<ExecutionPlan>) needs nothing more than this
// forward declaration, which keeps the bytecode layout and its heavy
// dependencies out of storage-only consumers.
//
// To build a plan, include <ao/query/QueryCompilation.h>. To inspect the bytecode
// layout directly (engine internals and white-box tests), include
// <ao/query/detail/Bytecode.h>.

#include <cstdint>

namespace ao::query
{
  struct ExecutionPlan;
  enum class AccessProfile : std::uint8_t;

  AccessProfile accessProfile(ExecutionPlan const& plan) noexcept;
} // namespace ao::query
