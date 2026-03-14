/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <rs/core/Dictionary.h>
#include <rs/core/TrackLayout.h>
#include <rs/expr/Expression.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace rs::expr
{

  /**
   * Field - Identifies which field to read from a track.
   */
  enum class Field : std::uint8_t
  {
    TagBloom = 0,
    DurationMs = 1,
    Bitrate = 2,
    SampleRate = 3,
    ArtistId = 4,
    AlbumId = 5,
    GenreId = 6,
    Year = 7,
    TrackNumber = 8,
    CodecId = 9,
    Channels = 10,
    BitDepth = 11,
    Rating = 12,
    TagCount = 13,
    Title = 14,
    Uri = 15,
    Tag = 16,
  };

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
  };

  /**
   * Instruction - A single operation in the execution plan.
   */
  struct Instruction
  {
    OpCode op;
    std::uint8_t field;   // Field index (for LoadField)
    std::int32_t operand; // For binary ops: register of left operand. For load: target register

    // For constants: stores the constant value directly
    std::int64_t constValue;

    // For string constants, we store the length and a pointer to the data
    // The actual string data will be stored separately in the plan
    std::uint32_t strLen;
    const char* strData;
  };

  /**
   * ExecutionPlan - Compiled query ready for fast execution.
   */
  class ExecutionPlan
  {
  public:
    std::vector<Instruction> instructions;
    std::vector<std::string> stringConstants;

    // Bloom filter for tag fast-path rejection
    std::uint64_t tagBloomMask = 0;

    // If true, the query matches all tracks (no conditions)
    bool matchesAll = false;
  };

  /**
   * QueryCompiler - Compiles an AST expression into an ExecutionPlan.
   *
   * Uses IDictionary to resolve string constants to numeric IDs for metadata fields
   * (artist, album, genre) to enable efficient numeric comparison at evaluation time.
   */
  class QueryCompiler
  {
  public:
    explicit QueryCompiler() = default;

    /**
     * Construct with a dictionary for string resolution.
     *
     * @param dict Dictionary for resolving string constants to IDs
     */
    explicit QueryCompiler(const core::IDictionary& dict);

    /**
     * Compile an expression AST into an execution plan.
     *
     * @param expr The expression AST to compile
     * @return Compiled execution plan
     */
    ExecutionPlan compile(const Expression& expr);

  private:
    std::uint32_t addStringConstant(std::string_view str);

    void compileExpression(const Expression& expr);
    void compileBinary(const BinaryExpression& binary);
    void compileUnary(const UnaryExpression& unary);
    void compileVariable(const VariableExpression& var);
    void compileConstant(const ConstantExpression& constant);

    // Resolve string to ID using dictionary (if available)
    std::int64_t resolveStringConstant(const std::string& str, Field field) const;

    ExecutionPlan _plan;
    std::uint32_t _nextReg = 0;
    const core::IDictionary* _dict = nullptr;
    Field _lastField = Field::TagBloom; // Track last field for context
  };

} // namespace rs::expr
