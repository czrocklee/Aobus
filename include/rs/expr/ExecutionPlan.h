// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/DictionaryStore.h>
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

    // Custom field (for %custom_key lookups from cold storage)
    Custom = 22,
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
   * AccessProfile - Indicates which storage tier(s) the query accesses.
   */
  enum class AccessProfile : std::uint8_t
  {
    HotOnly,   // Only accesses hot data (metadata, property, tags)
    ColdOnly,  // Only accesses cold data (custom KV)
    HotAndCold // Mixed access
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
    std::uint32_t tagBloomMask = 0;

    // If true, the query matches all tracks (no conditions)
    bool matchesAll = false;

    // Access profile for the query
    AccessProfile accessProfile = AccessProfile::HotOnly;
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
     * Construct with a DictionaryStore for string resolution.
     *
     * @param dict Pointer to DictionaryStore for resolving string constants to IDs, can be nullptr
     */
    explicit QueryCompiler(core::DictionaryStore* dict);

    /**
     * Compile an expression AST into an execution plan.
     *
     * @param expr The expression AST to compile
     * @return Compiled execution plan
     */
    ExecutionPlan compile(Expression const& expr);

  private:
    // Compile helper functions
    std::uint32_t addStringConstant(std::string_view str);
    void compileExpression(Expression const& expr);
    void compileBinary(BinaryExpression const& binary);
    void compileUnary(UnaryExpression const& unary);
    void compileVariable(VariableExpression const& var);
    void compileConstant(ConstantExpression const& constant);

    // Resolve string to ID using dictionary (if available)
    std::int64_t resolveStringConstant(std::string const& str, Field field);

    // Member variables
    ExecutionPlan _plan;
    std::uint32_t _nextReg = 0;
    core::DictionaryStore* _dict = nullptr;
    Field _lastField = Field::TagBloom; // Track last field for context
    bool _hasHotAccess = false;  // Track if expression uses hot (metadata/property/tag) variables
    bool _hasColdAccess = false; // Track if expression uses cold (custom) variables
  };

} // namespace rs::expr
