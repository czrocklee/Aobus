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

#include <rs/expr/ExecutionPlan.h>
#include <rs/utility/VariantVisitor.h>

#include <algorithm>

namespace rs::expr
{

  namespace
  {
    Field variableTypeToField(VariableType type, std::string_view name)
    {
      switch (type)
      {
        case VariableType::Property:
          if (name == "duration" || name == "l")
            return Field::DurationMs;
          if (name == "bitrate" || name == "br")
            return Field::Bitrate;
          if (name == "sampleRate" || name == "sr")
            return Field::SampleRate;
          if (name == "channels")
            return Field::Channels;
          if (name == "bitDepth" || name == "bd")
            return Field::BitDepth;
          break;
        case VariableType::Metadata:
          if (name == "year" || name == "y")
            return Field::Year;
          if (name == "trackNumber" || name == "tn")
            return Field::TrackNumber;
          if (name == "artist" || name == "a")
            return Field::ArtistId;
          if (name == "album" || name == "al")
            return Field::AlbumId;
          if (name == "genre" || name == "g")
            return Field::GenreId;
          if (name == "title" || name == "t")
            return Field::Title;
          break;
        case VariableType::Tag:
          return Field::Tag;
        default:
          break;
      }
      return Field::TagBloom;
    }

    OpCode toOpCode(Operator op)
    {
      switch (op)
      {
        case Operator::And:
          return OpCode::And;
        case Operator::Or:
          return OpCode::Or;
        case Operator::Not:
          return OpCode::Not;
        case Operator::Equal:
          return OpCode::Eq;
        case Operator::Like:
          return OpCode::Like;
        case Operator::Less:
          return OpCode::Lt;
        case Operator::LessEqual:
          return OpCode::Le;
        case Operator::Greater:
          return OpCode::Gt;
        case Operator::GreaterEqual:
          return OpCode::Ge;
        default:
          return OpCode::Nop;
      }
    }

    bool isMetadataFieldId(Field field)
    {
      return field == Field::ArtistId || field == Field::AlbumId || field == Field::GenreId;
    }

    bool isTagField(Field field) { return field == Field::Tag; }
  }

  QueryCompiler::QueryCompiler(const core::IDictionary& dict) : _dict(&dict) {}

  std::uint32_t QueryCompiler::addStringConstant(std::string_view str)
  {
    auto it = std::find(_plan.stringConstants.begin(), _plan.stringConstants.end(), std::string(str));
    if (it != _plan.stringConstants.end())
    {
      return static_cast<std::uint32_t>(std::distance(_plan.stringConstants.begin(), it));
    }
    _plan.stringConstants.emplace_back(str);
    return static_cast<std::uint32_t>(_plan.stringConstants.size() - 1);
  }

  void QueryCompiler::compileExpression(const Expression& expr)
  {
    boost::apply_visitor(
      utility::makeVisitor([this](const BinaryExpression& binary) { compileBinary(binary); },
                           [this](const UnaryExpression& unary) { compileUnary(unary); },
                           [this](const VariableExpression& var) { compileVariable(var); },
                           [this](const ConstantExpression& constant) { compileConstant(constant); }),
      expr);
  }

  void QueryCompiler::compileBinary(const BinaryExpression& binary)
  {
    // Compile left operand
    compileExpression(binary.operand);

    if (binary.operation)
    {
      // Compile right operand
      compileExpression(binary.operation->operand);

      // Right operand result is in _nextReg - 1
      // Binary op will store result in operand - 1 = (_nextReg - 1) - 1 = _nextReg - 2
      auto rightReg = _nextReg - 1;

      auto opcode = toOpCode(binary.operation->op);
      Instruction instr;
      instr.op = opcode;
      instr.field = 0;
      instr.operand = rightReg;
      instr.strLen = 0;
      instr.strData = nullptr;
      _plan.instructions.push_back(instr);

      // After binary op, the result is stored in the left register (rightReg - 1)
      // The right register is now free, so decrement _nextReg
      _nextReg--;
    }
  }

  void QueryCompiler::compileUnary(const UnaryExpression& unary)
  {
    compileExpression(unary.operand);

    auto opcode = toOpCode(unary.op);
    Instruction instr;
    instr.op = opcode;
    instr.field = 0;
    instr.operand = static_cast<std::int32_t>(_nextReg - 1);
    instr.strLen = 0;
    instr.strData = nullptr;
    _plan.instructions.push_back(instr);
  }

  void QueryCompiler::compileVariable(const VariableExpression& var)
  {
    if (var.type == VariableType::Tag)
    {
      // Try to resolve tag name to ID via dictionary for bloom filter
      if (_dict)
      {
        auto tagId = _dict->getStringId(var.name);
        if (tagId.value() > 0)
        {
          _plan.tagBloomMask |= (1U << (tagId.value() & 31));

          // Generate implicit tag comparison: track.tags().has(tagId)
          // This handles standalone "#tagname" queries like "#rock"
          // First, load the tag field (for the Eq instruction to detect it's a tag comparison)
          Instruction loadField;
          loadField.op = OpCode::LoadField;
          loadField.field = static_cast<std::uint8_t>(Field::Tag);
          loadField.operand = static_cast<std::int32_t>(_nextReg++);
          loadField.strLen = 0;
          loadField.strData = nullptr;
          _plan.instructions.push_back(loadField);

          // Then load the tag ID as constant
          Instruction loadConst;
          loadConst.op = OpCode::LoadConstant;
          loadConst.field = 0;
          loadConst.operand = static_cast<std::int32_t>(_nextReg++);
          loadConst.constValue = static_cast<std::int64_t>(tagId.value());
          loadConst.strLen = 0;
          loadConst.strData = nullptr;
          _plan.instructions.push_back(loadConst);

          // Eq instruction - PlanEvaluator will detect Tag field and use tags.has()
          Instruction eqInstr;
          eqInstr.op = OpCode::Eq;
          eqInstr.field = 0;
          eqInstr.operand = static_cast<std::int32_t>(_nextReg - 1); // Right operand
          eqInstr.strLen = 0;
          eqInstr.strData = nullptr;
          _plan.instructions.push_back(eqInstr);

          _nextReg--; // Eq result is in the left register
          return;
        }
      }
    }

    auto field = variableTypeToField(var.type, var.name);
    _lastField = field; // Track for string resolution context

    Instruction instr;
    instr.op = OpCode::LoadField;
    instr.field = static_cast<std::uint8_t>(field);
    instr.operand = static_cast<std::int32_t>(_nextReg++);
    instr.strLen = 0;
    instr.strData = nullptr;
    _plan.instructions.push_back(instr);
  }

  void QueryCompiler::compileConstant(const ConstantExpression& constant)
  {
    std::visit(utility::makeVisitor(
                 [this](bool val) {
                   Instruction instr;
                   instr.op = OpCode::LoadConstant;
                   instr.field = 0;
                   instr.operand = static_cast<std::int32_t>(_nextReg++);
                   instr.constValue = val ? 1 : 0;
                   instr.strLen = 0;
                   instr.strData = nullptr;
                   _plan.instructions.push_back(instr);
                 },
                 [this](std::int64_t val) {
                   Instruction instr;
                   instr.op = OpCode::LoadConstant;
                   instr.field = 0;
                   instr.operand = static_cast<std::int32_t>(_nextReg++);
                   instr.constValue = val;
                   instr.strLen = 0;
                   instr.strData = nullptr;
                   _plan.instructions.push_back(instr);
                 },
                 [this](const std::string& val) {
                   // Check if we should resolve this string via dictionary
                   // For metadata ID fields (artist, album, genre), resolve to numeric ID
                   auto resolvedId = resolveStringConstant(val, _lastField);

                   Instruction instr;
                   instr.op = OpCode::LoadConstant;
                   instr.field = 0;
                   instr.operand = static_cast<std::int32_t>(_nextReg++);

                   if (resolvedId >= 0)
                   {
                     // Successfully resolved to ID - store as numeric constant
                     instr.constValue = resolvedId;
                     instr.strLen = 0;
                   }
                   else
                   {
                     // Not resolved (no dictionary or not a metadata ID field) - store as string constant
                     auto idx = addStringConstant(val);
                     instr.constValue = static_cast<std::int64_t>(idx);
                     instr.strLen = static_cast<std::uint32_t>(val.size());
                   }
                   instr.strData = nullptr;
                   _plan.instructions.push_back(instr);
                 }),
               constant);
  }

  std::int64_t QueryCompiler::resolveStringConstant(const std::string& str, Field field) const
  {
    // Only resolve for metadata ID fields and tag fields
    if (!isMetadataFieldId(field) && !isTagField(field))
    {
      return -1; // Not applicable
    }

    if (!_dict)
    {
      return -1; // No dictionary available
    }

    // Look up the string in the dictionary
    auto id = _dict->getStringId(str);
    if (id.value() == 0)
    {
      return -1; // String not found in dictionary
    }

    return static_cast<std::int64_t>(id.value());
  }

  ExecutionPlan QueryCompiler::compile(const Expression& expr)
  {
    _plan = ExecutionPlan{};
    _nextReg = 0;

    // Check if the expression is a constant "true"
    if (const auto* constant = boost::get<ConstantExpression>(&expr))
    {
      if (const bool* val = std::get_if<bool>(constant))
      {
        if (*val)
        {
          _plan.matchesAll = true;
        }
      }
    }

    compileExpression(expr);

    return _plan;
  }

} // namespace rs::expr
