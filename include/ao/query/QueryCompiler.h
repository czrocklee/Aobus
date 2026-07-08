// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::library
{
  class DictionaryStore;
}

namespace ao::query
{
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
     * @param dictionary Pointer to DictionaryStore for resolving string constants to IDs, can be nullptr
     */
    explicit QueryCompiler(library::DictionaryStore* dictionary);

    /**
     * Compile an expression AST into an execution plan.
     *
     * @param query The expression AST to compile
     * @return Compiled execution plan
     */
    Result<ExecutionPlan> compile(Expression const& expr);

  private:
    enum class InSetValueStatus : std::uint8_t
    {
      NotCompatible,
      Appended,
    };

    // Register-stack helpers. Each value-producing op claims the next register up and
    // each consumed (right) operand is always the current top, so the evaluator can
    // read a binary op's left operand from (operand - 1). These two helpers keep that
    // stack discipline in one place instead of open-coding _nextReg arithmetic.
    std::uint32_t pushReg();
    void popReg(std::uint32_t top);

    // Compile helper functions. Each returns the register holding its result (with
    // _nextReg left incremented by exactly one net), so callers thread registers
    // explicitly rather than re-deriving them from _nextReg. compileInSetList returns
    // nullopt when the list is not eligible for set compilation.
    std::uint32_t addStringConstant(std::string_view str);
    std::uint32_t addInSet(InSet set);
    std::uint32_t compileExpression(Expression const& expr);
    std::uint32_t compilePredicate(Expression const& expr);
    std::uint32_t compileBinary(BinaryExpression const& binary);
    std::uint32_t compileUnary(UnaryExpression const& unary);
    std::uint32_t compileExists(Expression const& operand);
    std::uint32_t compileVariable(VariableExpression const& var);
    std::uint32_t compileConstant(ConstantExpression const& constant);
    std::uint32_t compileList(ListExpression const& list);
    std::uint32_t compileRange(RangeExpression const& range);
    std::uint32_t compileIn(Expression const& lhs, Expression const& rhs);
    std::uint32_t compileInWithList(Expression const& lhs, ListExpression const& list);
    std::uint32_t compileInRange(Expression const& lhs, RangeExpression const& range);
    std::optional<std::uint32_t> compileInSetList(Expression const& lhs, ListExpression const& list);

    // Resolve string to ID using dictionary (if available)
    std::int64_t resolveStringConstant(std::string const& str, Field field);
    InSetValueStatus appendInSetValue(InSet& set, ConstantExpression const& constant, Field field);

    // Member variables
    ExecutionPlan _plan;
    std::uint32_t _nextReg = 0;
    library::DictionaryStore* _dictionary = nullptr;
    Field _lastField = Field::TagBloom; // Track last field for context
    // Interned dictionaryId of the last field when it is Field::Custom (0 otherwise). The
    // comparison/membership instruction carries this so the evaluator can resolve the
    // custom key without a separate LoadField lookup.
    std::int64_t _lastFieldCustomId = 0;
    bool _hasHotAccess = false;  // Track if expression uses hot (metadata/property/tag) variables
    bool _hasColdAccess = false; // Track if expression uses cold (custom) variables
    bool _resolveStringConstantsToIds = true;
  };

  /**
   * Compile an expression AST into an execution plan (non-throwing entry point).
   *
   * @param expr The expression AST to compile.
   * @param dictionary Optional DictionaryStore for resolving string constants to IDs; may be nullptr.
   * @return The compiled ExecutionPlan, or an Error{Code::FormatRejected, ...} if @p expr is not
   *         a valid query predicate. Never throws on invalid input.
   */
  Result<ExecutionPlan> compileQuery(Expression const& expr, library::DictionaryStore* dictionary = nullptr);
} // namespace ao::query
