// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <cstdint>
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
     * @param dict Pointer to DictionaryStore for resolving string constants to IDs, can be nullptr
     */
    explicit QueryCompiler(library::DictionaryStore* dict);

    /**
     * Compile an expression AST into an execution plan.
     *
     * @param query The expression AST to compile
     * @return Compiled execution plan
     */
    Result<ExecutionPlan> compile(Expression const& expr);

  private:
    enum class InSetCompileStatus : std::uint8_t
    {
      NotApplicable,
      Compiled,
    };

    enum class InSetValueStatus : std::uint8_t
    {
      NotCompatible,
      Appended,
    };

    // Compile helper functions
    std::uint32_t addStringConstant(std::string_view str);
    std::uint32_t addInSet(InSet set);
    Result<> compileExpression(Expression const& expr);
    Result<> compilePredicate(Expression const& expr);
    Result<> compileBinary(BinaryExpression const& binary);
    Result<> compileUnary(UnaryExpression const& unary);
    Result<> compileExists(Expression const& operand);
    Result<> compileVariable(VariableExpression const& var);
    Result<> compileConstant(ConstantExpression const& constant);
    Result<> compileList(ListExpression const& list);
    Result<> compileRange(RangeExpression const& range);
    Result<> compileIn(Expression const& lhs, Expression const& rhs);
    Result<InSetCompileStatus> compileInSetList(Expression const& lhs, ListExpression const& list);

    // Resolve string to ID using dictionary (if available)
    std::int64_t resolveStringConstant(std::string const& str, Field field);
    Result<InSetValueStatus> appendInSetValue(InSet& set, ConstantExpression const& constant, Field field);

    // Member variables
    ExecutionPlan _plan;
    std::uint32_t _nextReg = 0;
    library::DictionaryStore* _dict = nullptr;
    Field _lastField = Field::TagBloom; // Track last field for context
    bool _hasHotAccess = false;         // Track if expression uses hot (metadata/property/tag) variables
    bool _hasColdAccess = false;        // Track if expression uses cold (custom) variables
    bool _resolveStringConstantsToIds = true;
  };

  /**
   * Compile an expression AST into an execution plan (non-throwing entry point).
   *
   * @param expr The expression AST to compile.
   * @param dict Optional DictionaryStore for resolving string constants to IDs; may be nullptr.
   * @return The compiled ExecutionPlan, or an Error{Code::FormatRejected, ...} if @p expr is not
   *         a valid query predicate. Never throws on invalid input.
   */
  Result<ExecutionPlan> compileQuery(Expression const& expr, library::DictionaryStore* dict = nullptr);
} // namespace ao::query
