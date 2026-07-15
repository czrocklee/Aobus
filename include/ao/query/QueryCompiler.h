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

namespace ao::query
{
  /**
   * QueryCompiler - Compiles an AST expression into an ExecutionPlan.
   *
   * Dictionary-backed names and constants compile to plan-owned text symbols.
   * A PlanBinding resolves those symbols for one evaluation context.
   */
  class QueryCompiler final
  {
  public:
    explicit QueryCompiler() = default;

    /**
     * Compile an expression AST into an execution plan.
     *
     * @param query The expression AST to compile
     * @return Compiled execution plan
     */
    Result<ExecutionPlan> compile(Expression const& expr);

  private:
    struct CompiledConstant final
    {
      std::uint32_t reg = 0;
      std::uint32_t dictionarySymbol = kNoDictionarySymbol;
    };

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
    std::uint32_t addDictionarySymbol(std::string_view text);
    std::uint32_t addInSet(InSet set);
    std::uint32_t compileExpression(Expression const& expr);
    std::uint32_t compilePredicate(Expression const& expr);
    std::uint32_t compileBinary(BinaryExpression const& binary);
    std::uint32_t compileUnary(UnaryExpression const& unary);
    std::uint32_t compileExists(Expression const& operand);
    std::uint32_t compileVariable(VariableExpression const& var);
    CompiledConstant compileConstant(ConstantExpression const& constant);
    std::uint32_t compileList(ListExpression const& list);
    std::uint32_t compileRange(RangeExpression const& range);
    std::uint32_t compileIn(Expression const& lhs, Expression const& rhs);
    std::uint32_t compileInWithList(Expression const& lhs, ListExpression const& list);
    std::uint32_t compileInRange(Expression const& lhs, RangeExpression const& range);
    std::optional<std::uint32_t> compileInSetList(Expression const& lhs, ListExpression const& list);

    std::optional<std::uint32_t> dictionarySymbolForStringConstant(std::string const& str, Field field);
    InSetValueStatus appendInSetValue(InSet& set, ConstantExpression const& constant, Field field);

    // Member variables
    ExecutionPlan _plan;
    std::uint32_t _nextReg = 0;
    Field _lastField = Field::TagBloom; // Track last field for context
    std::uint32_t _lastFieldCustomSymbol = kNoDictionarySymbol;
    bool _hasHotAccess = false;  // Track if expression uses hot (metadata/property/tag) variables
    bool _hasColdAccess = false; // Track if expression uses cold (custom) variables
    bool _hasDictionaryAccess = false;
    bool _resolveStringConstantsToIds = true;
  };

  /**
   * Compile an expression AST into an execution plan (non-throwing entry point).
   *
   * @param expr The expression AST to compile.
   * @return The compiled ExecutionPlan, or an Error{Code::FormatRejected, ...} if @p expr is not
   *         a valid query predicate. Never throws on invalid input.
   */
  Result<ExecutionPlan> compileQuery(Expression const& expr);
} // namespace ao::query
