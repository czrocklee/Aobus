// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Internal query-engine bytecode layout. This header carries the heavy
// dependencies (boost flat_set) and the concrete ExecutionPlan
// fields; the public <ao/query/ExecutionPlan.h> only forward-declares the plan
// as an opaque handle. Include this header only from the query engine internals
// (compiler/evaluator) and white-box tests that inspect compiled bytecode.

#include <ao/query/Field.h>

#include <boost/unordered/unordered_flat_set.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ao::query
{
  /**
   * OpCode - Operations in the execution plan.
   */
  enum class OpCode : std::uint8_t
  {
    Nop = 0,
    LoadField,
    LoadConstant,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Not,
    Like,
    Exists,
    InSet,
  };

  // An ordered comparison resolves dictionary IDs back to text before comparing;
  // both the compiler and the evaluator need this classification, so it lives here
  // beside the OpCode definition rather than being duplicated in each.
  constexpr bool isOrderedComparison(OpCode op)
  {
    return op == OpCode::Lt || op == OpCode::Le || op == OpCode::Gt || op == OpCode::Ge;
  }

  inline constexpr std::uint32_t kNoDictionarySymbol = std::numeric_limits<std::uint32_t>::max();

  enum class InSetValueKind : std::uint8_t
  {
    Numeric,
    String,
    Dictionary,
  };

  struct InSet final
  {
    InSetValueKind valueKind = InSetValueKind::Numeric;
    boost::unordered_flat_set<std::int64_t> numericValues;
    std::vector<std::string> strings;
    std::vector<std::uint32_t> dictionarySymbols;
  };

  /**
   * Instruction - A single operation in the execution plan.
   *
   * Field-bearing ops (LoadField, the comparisons, Like, Exists, InSet) carry their
   * left Field directly in `field`, so the evaluator never has to scan back for the
   * nearest LoadField to recover an operand's type.
   */
  struct Instruction final
  {
    OpCode op = OpCode::Nop;

    // Left field for field-bearing ops; 0/unused for logical ops (And/Or/Not) and
    // LoadConstant.
    std::uint8_t field = 0;

    // Binary/compare ops: register of the right operand (the result is written to
    // operand - 1). Load/Exists/InSet: target register.
    std::int32_t operand = 0;

    // LoadConstant: the constant value (or the string-constant index). InSet:
    // the index into ExecutionPlan::inSets. Unused by other field-bearing ops.
    std::int64_t constValue = 0;

    // Reserved for bytecode payloads that require an additional 32-bit value.
    std::uint32_t size = 0;

    // Plan-owned dictionary symbol used by tag/custom operations or dictionary-backed
    // equality. kNoDictionarySymbol means the instruction has no dictionary operand.
    std::uint32_t dictionarySymbol = kNoDictionarySymbol;
    char const* data = nullptr;
  };

  /**
   * ExecutionPlan - Compiled query ready for fast execution.
   *
   * Exposed to consumers as an opaque handle via <ao/query/ExecutionPlan.h>;
   * the concrete layout below is an internal detail of the query engine.
   */
  struct ExecutionPlan final
  {
    std::vector<Instruction> instructions;
    std::vector<std::string> stringConstants;
    std::vector<InSet> inSets;
    std::vector<std::string> dictionarySymbols;
    std::vector<std::uint32_t> requiredTagSymbols;

    // If true, the query matches all tracks (no conditions)
    bool matchesAll = false;

    // True when evaluation needs a DictionaryReadContext for symbol or id-to-text lookup.
    bool requiresDictionary = false;

    // Access profile for the query
    AccessProfile accessProfile = AccessProfile::HotOnly;
  };
} // namespace ao::query
