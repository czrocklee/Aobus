// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Internal query-engine bytecode layout. This header carries the heavy
// dependencies and the concrete ExecutionPlan fields; the public
// <ao/query/ExecutionPlan.h> only forward-declares the plan as an opaque
// handle. <ao/query/QueryCompilation.h> includes this layout because its
// Result<ExecutionPlan> boundary returns the plan by value. Other application
// code should not include this header directly; query-engine internals and
// white-box tests may include it to inspect compiled bytecode.

#include <ao/library/RecordingDate.h>
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
    CompareText,
  };

  // An ordered comparison resolves dictionary IDs back to text before comparing;
  // both the compiler and the evaluator need this classification, so it lives here
  // beside the OpCode definition rather than being duplicated in each.
  constexpr bool isOrderedComparison(OpCode op)
  {
    return op == OpCode::Lt || op == OpCode::Le || op == OpCode::Gt || op == OpCode::Ge;
  }

  inline constexpr std::uint32_t kNoDictionarySymbol = std::numeric_limits<std::uint32_t>::max();

  // This shares the compiler's kNoFieldContext encoding; the operand kind distinguishes a text literal there.
  // In CompareText it denotes a string-constant index in the operand register, not a track's tag bloom filter.
  inline constexpr Field kTextLiteralField = Field::TagBloom;

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
   * nearest LoadField to recover an operand's type. CompareText also carries the
   * right operand's field and the relation to apply to the two text values.
   */
  struct Instruction final
  {
    OpCode op = OpCode::Nop;

    // Left field for field-bearing ops; 0/unused for logical ops (And/Or/Not) and
    // LoadConstant.
    std::uint8_t field = 0;

    // CompareText resolves both operands as text before applying Eq/Ne/Lt/Le/Gt/Ge.
    // Either field can be kTextLiteralField to read a literal from its register.
    Field rightTextField = kTextLiteralField;
    OpCode textRelation = OpCode::Eq;

    // Binary/compare ops: register of the right operand (the result is written to
    // operand - 1). Load/Exists/InSet: target register.
    std::int32_t operand = 0;

    // LoadConstant: the scalar value, string-constant index, or typed
    // recording-date constant index. InSet: the index into ExecutionPlan::inSets.
    // Unused by other field-bearing ops.
    std::int64_t constValue = 0;

    // CompareText: custom-key symbol for the right field, if any.
    std::uint32_t rightDictionarySymbol = kNoDictionarySymbol;

    // Plan-owned dictionary symbol used by tag/custom operations or dictionary-backed
    // equality. kNoDictionarySymbol means there is no symbol in this slot;
    // CompareText may still need a right-hand key or an ID-to-text lookup.
    std::uint32_t dictionarySymbol = kNoDictionarySymbol;
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
    std::vector<library::RecordingDate> recordingDateConstants;
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
