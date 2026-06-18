// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/DictionaryStore.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>

#include <boost/unordered/unordered_flat_set.hpp>

#include <cstdint>
#include <string>
#include <string_view>
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

  struct InSet final
  {
    bool stringValues = false;
    boost::unordered_flat_set<std::int64_t> numericValues;
    std::vector<std::string> strings;
  };

  /**
   * Instruction - A single operation in the execution plan.
   */
  struct Instruction final
  {
    OpCode op = OpCode::Nop;
    std::uint8_t field = 0;   // Field index (for LoadField)
    std::int32_t operand = 0; // For binary ops: register of left operand. For load: target register

    // For constants: stores the constant value directly
    std::int64_t constValue = 0;

    // For string constants, we store the length and a pointer to the data
    // The actual string data will be stored separately in the plan
    std::uint32_t size = 0;
    char const* data = nullptr;
  };

  /**
   * ExecutionPlan - Compiled query ready for fast execution.
   */
  struct ExecutionPlan final
  {
    std::vector<Instruction> instructions;
    std::vector<std::string> stringConstants;
    std::vector<InSet> inSets;

    // For each instruction, the index of the nearest preceding LoadField (or -1).
    // This is a static property of the instruction stream, so it is computed once
    // here (via indexFieldLoads) rather than rescanned per track at evaluation time.
    // Empty when not yet populated; the evaluator derives it on the fly in that case.
    std::vector<std::int32_t> fieldLoadIndex;

    // Dictionary used to resolve DictionaryId-backed metadata during evaluation.
    library::DictionaryStore const* dictionary = nullptr;

    // Bloom filter for tag fast-path rejection
    std::uint32_t tagBloomMask = 0;

    // If true, the query matches all tracks (no conditions)
    bool matchesAll = false;

    // Access profile for the query
    AccessProfile accessProfile = AccessProfile::HotOnly;

    // Populate fieldLoadIndex from the current instruction stream. Idempotent;
    // call after instructions are finalized so evaluation can resolve a comparison's
    // field operand in O(1) without rescanning.
    void indexFieldLoads();
  };

  /**
   * QueryCompiler - Compiles an AST expression into an ExecutionPlan.
   *
   * Uses Dictionary to resolve string constants to numeric IDs for metadata fields
   * (artist, album, genre) to enable efficient numeric comparison at evaluation time.
   */
  class QueryCompiler final
  {
  public:
    explicit QueryCompiler() = default;

    /**
     * Construct with a DictionaryStore for string resolution.
     *
     * @param dict Pointer to DictionaryStore for resolving string constants to IDs, can be nullptr
     */
    explicit QueryCompiler(library::DictionaryStore* dict);

    /**
     * Compile an expression AST into an execution plan.
     *
     * @param query The expression AST to compile
     * @return Compiled execution plan
     */
    ExecutionPlan compile(Expression const& expr);

  private:
    // Compile helper functions
    std::uint32_t addStringConstant(std::string_view str);
    std::uint32_t addInSet(InSet set);
    void compileExpression(Expression const& expr);
    void compilePredicate(Expression const& expr);
    void compileBinary(BinaryExpression const& binary);
    void compileUnary(UnaryExpression const& unary);
    void compileExists(Expression const& operand);
    void compileVariable(VariableExpression const& var);
    void compileConstant(ConstantExpression const& constant);
    void compileList(ListExpression const& list);
    void compileRange(RangeExpression const& range);
    void compileIn(Expression const& lhs, Expression const& rhs);
    bool compileInSetList(Expression const& lhs, ListExpression const& list);

    // Resolve string to ID using dictionary (if available)
    std::int64_t resolveStringConstant(std::string const& str, Field field);
    bool appendInSetValue(InSet& set, ConstantExpression const& constant, Field field);

    // Member variables
    ExecutionPlan _plan;
    std::uint32_t _nextReg = 0;
    library::DictionaryStore* _dict = nullptr;
    Field _lastField = Field::TagBloom; // Track last field for context
    bool _hasHotAccess = false;         // Track if expression uses hot (metadata/property/tag) variables
    bool _hasColdAccess = false;        // Track if expression uses cold (custom) variables
    bool _resolveStringConstantsToIds = true;
  };
} // namespace ao::query
