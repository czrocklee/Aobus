// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::query
{
  enum class FormatOpCode : std::uint8_t
  {
    AppendLiteral,
    AppendField,
  };

  struct FormatInstruction final
  {
    FormatOpCode op = FormatOpCode::AppendLiteral;
    Field field = Field::Title;
    std::int64_t constValue = 0;
    std::uint32_t literalIndex = 0;
  };

  struct FormatPlan final
  {
    std::vector<FormatInstruction> instructions;
    std::vector<std::string> literals;
    library::DictionaryStore const* dictionary = nullptr;
    AccessProfile accessProfile = AccessProfile::NoTrackData;
  };

  class FormatCompiler final
  {
  public:
    explicit FormatCompiler() = default;
    explicit FormatCompiler(library::DictionaryStore* dictionary);

    Result<FormatPlan> compile(Expression const& expr);

  private:
    std::uint32_t addLiteral(std::string_view value);
    void compileExpression(Expression const& expr);
    void compileBinary(BinaryExpression const& binary);
    void compileVariable(VariableExpression const& variable);
    void compileConstant(ConstantExpression const& constant);

    FormatPlan _plan;
    library::DictionaryStore* _dictionary = nullptr;
    bool _hasHotAccess = false;
    bool _hasColdAccess = false;
  };

  class FormatEvaluator final
  {
  public:
    std::string evaluate(FormatPlan const& plan, library::TrackView const& track) const;

    /**
     * Evaluate into caller-owned storage.
     *
     * Clears @p output before evaluation, including when @p track does not
     * provide the data required by @p plan. Retained string capacity may be
     * reused by later calls.
     */
    void evaluate(FormatPlan const& plan, library::TrackView const& track, std::string& output) const;
  };

  /**
   * Compile a format expression AST into a FormatPlan (non-throwing entry point).
   *
   * @param expr The expression AST to compile.
   * @param dictionary Optional DictionaryStore for resolving dictionary-backed fields; may be nullptr.
   * @return The compiled FormatPlan, or an Error{Code::FormatRejected, ...} if @p expr is not a
   *         valid format expression. Never throws on invalid input.
   */
  Result<FormatPlan> compileFormat(Expression const& expr, library::DictionaryStore* dictionary = nullptr);
} // namespace ao::query
