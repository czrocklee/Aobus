// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/TrackView.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library
{
  class DictionaryReadContext;
}

namespace ao::query
{
  inline constexpr std::uint32_t kNoFormatDictionarySymbol = std::numeric_limits<std::uint32_t>::max();

  enum class FormatOpCode : std::uint8_t
  {
    AppendLiteral,
    AppendField,
  };

  struct FormatInstruction final
  {
    FormatOpCode op = FormatOpCode::AppendLiteral;
    Field field = Field::Title;
    std::uint32_t literalIndex = 0;
    std::uint32_t dictionarySymbol = kNoFormatDictionarySymbol;
  };

  struct FormatPlan final
  {
    std::vector<FormatInstruction> instructions;
    std::vector<std::string> literals;
    std::vector<std::string> dictionarySymbols;
    bool requiresDictionary = false;
    AccessProfile accessProfile = AccessProfile::NoTrackData;
  };

  class FormatCompiler final
  {
  public:
    explicit FormatCompiler() = default;

    Result<FormatPlan> compile(Expression const& expr);

  private:
    std::uint32_t addLiteral(std::string_view value);
    std::uint32_t addDictionarySymbol(std::string_view text);
    void compileExpression(Expression const& expr);
    void compileBinary(BinaryExpression const& binary);
    void compileVariable(VariableExpression const& variable);
    void compileConstant(ConstantExpression const& constant);

    FormatPlan _plan;
    bool _hasHotAccess = false;
    bool _hasColdAccess = false;
    bool _hasDictionaryAccess = false;
  };

  /**
   * Resolves one immutable format plan against an optional dictionary context.
   *
   * The binding borrows the plan and, when supplied, the dictionary context; each
   * borrowed object must outlive the binding. Reuse one binding for a batch.
   */
  class FormatBinding final
  {
  public:
    /// @pre @p plan outlives the binding and is context-free (`requiresDictionary == false`).
    explicit FormatBinding(FormatPlan const& plan);

    /// @pre @p plan and @p dictionary both outlive the binding.
    FormatBinding(FormatPlan const& plan, library::DictionaryReadContext& dictionary);
    ~FormatBinding();

    FormatBinding(FormatBinding const&) = delete;
    FormatBinding& operator=(FormatBinding const&) = delete;
    FormatBinding(FormatBinding&&) noexcept;
    FormatBinding& operator=(FormatBinding&&) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class FormatEvaluator;
  };

  class FormatEvaluator final
  {
  public:
    /// @pre @p track provides every storage tier required by the bound plan's access profile.
    std::string evaluate(FormatBinding const& binding, library::TrackView const& track) const;

    /**
     * Convenience evaluation for a context-free plan.
     *
     * Requires `plan.requiresDictionary == false`. This constructs a binding per
     * call; reuse FormatBinding when evaluating a batch.
     */
    /// @pre `plan.requiresDictionary == false` and @p track satisfies `plan.accessProfile`.
    std::string evaluate(FormatPlan const& plan, library::TrackView const& track) const;

    /**
     * Evaluate into caller-owned storage.
     *
     * Clears @p output before evaluation. Retained string capacity may be reused
     * by later calls.
     *
     * @pre @p track provides every storage tier required by the bound plan's access profile.
     */
    void evaluate(FormatBinding const& binding, library::TrackView const& track, std::string& output) const;

    /// @pre `plan.requiresDictionary == false` and @p track satisfies `plan.accessProfile`.
    void evaluate(FormatPlan const& plan, library::TrackView const& track, std::string& output) const;
  };

  /**
   * Compile a format expression AST into a FormatPlan (non-throwing entry point).
   *
   * @param expr The expression AST to compile.
   * @return The compiled FormatPlan, or an Error{Code::FormatRejected, ...} if @p expr is not a
   *         valid format expression. Never throws on invalid input.
   */
  Result<FormatPlan> compileFormat(Expression const& expr);
} // namespace ao::query
