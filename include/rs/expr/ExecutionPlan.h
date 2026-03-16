// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

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
   * Ordered by category: string -> property -> metadata -> tags
   */
  enum class Field : std::uint8_t
  {
    // String fields
    Title = 0,
    Uri = 1,

    // Property fields (@ prefix) - audio technical properties
    DurationMs = 2,
    Bitrate = 3,
    SampleRate = 4,
    Channels = 5,
    BitDepth = 6,
    CodecId = 7,
    Rating = 8,

    // Metadata ID fields (Dictionary IDs)
    ArtistId = 9,
    AlbumId = 10,
    GenreId = 11,
    AlbumArtistId = 12,
    CoverArtId = 13,

    // Metadata numeric fields
    Year = 14,
    TrackNumber = 15,
    TotalTracks = 16,
    DiscNumber = 17,
    TotalDiscs = 18,

    // Tag fields
    TagBloom = 19,
    TagCount = 20,
    Tag = 21,
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
  struct Instruction final
  {
    OpCode op;
    std::uint8_t field;   // Field index (for LoadField)
    std::int32_t operand; // For binary ops: register of left operand. For load: target register

    // For constants: stores the constant value directly
    std::int64_t constValue;

    // For string constants, we store the length and a pointer to the data
    // The actual string data will be stored separately in the plan
    std::uint32_t strLen;
    char const* strData;
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
   * Uses Dictionary to resolve string constants to numeric IDs for metadata fields
   * (artist, album, genre) to enable efficient numeric comparison at evaluation time.
   */
  class QueryCompiler
  {
  public:
    explicit QueryCompiler() = default;

    /**
     * Construct with a dictionary for string resolution.
     *
     * @param dict Pointer to Dictionary for resolving string constants to IDs, can be nullptr
     */
    explicit QueryCompiler(core::Dictionary const* dict);

    /**
     * Compile an expression AST into an execution plan.
     *
     * @param expr The expression AST to compile
     * @return Compiled execution plan
     */
    [[nodiscard]] ExecutionPlan compile(Expression const& expr);

  private:
    std::uint32_t addStringConstant(std::string_view str);

    void compileExpression(Expression const& expr);
    void compileBinary(BinaryExpression const& binary);
    void compileUnary(UnaryExpression const& unary);
    void compileVariable(VariableExpression const& var);
    void compileConstant(ConstantExpression const& constant);

    // Resolve string to ID using dictionary (if available)
    [[nodiscard]] std::int64_t resolveStringConstant(std::string const& str, Field field) const;

    ExecutionPlan _plan;
    std::uint32_t _nextReg = 0;
    core::Dictionary const* _dict = nullptr;
    Field _lastField = Field::TagBloom; // Track last field for context
  };

} // namespace rs::expr
