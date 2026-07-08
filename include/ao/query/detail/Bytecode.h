// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Internal query-engine bytecode layout. This header carries the heavy
// dependencies (boost flat_set, DictionaryStore) and the concrete ExecutionPlan
// fields; the public <ao/query/ExecutionPlan.h> only forward-declares the plan
// as an opaque handle. Include this header only from the query engine internals
// (compiler/evaluator) and white-box tests that inspect compiled bytecode.

#include <ao/library/DictionaryStore.h>
#include <ao/query/Field.h>

#include <boost/unordered/unordered_flat_set.hpp>

#include <cstdint>
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

  struct InSet final
  {
    bool stringValues = false;
    boost::unordered_flat_set<std::int64_t> numericValues;
    std::vector<std::string> strings;
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

    // LoadConstant: the constant value (or the string-constant index). Exists:
    // the custom/tag dictionaryId. InSet: the index into ExecutionPlan::inSets.
    // Comparisons/Like: the left field's interned dictionaryId when it is Field::Custom
    // (0 otherwise).
    std::int64_t constValue = 0;

    // InSet only: the left field's interned dictionaryId when it is Field::Custom. InSet
    // spends constValue on the set index, so the Custom id rides here instead.
    std::uint32_t size = 0;
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

    // Dictionary used to resolve DictionaryId-backed metadata during evaluation.
    library::DictionaryStore const* dictionary = nullptr;

    // Bloom filter for tag fast-path rejection
    std::uint32_t tagBloomMask = 0;

    // If true, the query matches all tracks (no conditions)
    bool matchesAll = false;

    // Access profile for the query
    AccessProfile accessProfile = AccessProfile::HotOnly;
  };
} // namespace ao::query
