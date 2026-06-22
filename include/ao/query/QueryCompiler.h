// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

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
