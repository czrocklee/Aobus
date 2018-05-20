/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <rs/ml/utility/CppCodeWriter.H>

#include <flatbuffers/reflection.h>
 
namespace rs::ml::query
{
  class TrackFieldAccessorGenerator
  {
    using CppCodeWriter = rs::ml::utility::CppCodeWriter;

  public:
    template<typename Fields>
    void operator()(std::ostream& os, const Fields& fields)
    {
      CppCodeWriter writer{os};

      writer | "// THIS IS A GENERATED FILE, DO NOT EDIT!!!"
             | ""
             | "#pragma once"
             | ""
             | "#include <rs/ml/core/Track.H>"
             | "#include <rs/ml/core/DataValue.H>"
             | "#include <unordered_map>"
             | ""
             | "class TrackFieldAccessor"
             | "{"
             | "public:";
                  generateName2IdFunction(writer, fields);
      writer | "";
                  generateTrackAccessor(writer, fields);
      writer | "";
                  generateTrackTAccessor(writer, fields);
      writer | "};";
    }

  private:
    template<typename Fields>
    void generateName2IdFunction(CppCodeWriter& writer, const Fields& fields)
    {
      auto indent = writer.indent();

      writer | "static int name2Id(const std::string& name)"
             | "{"
             | "  static std::unordered_map<std::string, int> name2IdMap = "
             | "  {";

      for (auto i = 0u; i < fields.size(); ++i)
      {
        writer ( "    {\"%1%\", %2%},", fields[i]->name()->c_str(), fields[i]->id()); 
        auto attributes = fields[i]->attributes();

        if (attributes != nullptr)
        {
          auto alias = attributes->LookupByKey("alias");
        
          if (alias != nullptr)
          {
            writer ( "    {\"%1%\", %2%},", alias->value()->c_str(), fields[i]->id()); 
          }
        }
      }

      writer | "  };"
             | ""
             | "  auto iter = name2IdMap.find(name);"
             | "  return iter != name2IdMap.end() ? iter->second : -1;"
             | "};";
    }

    template<typename Fields>
    void generateTrackAccessor(CppCodeWriter& writer, const Fields& fields)
    {
      auto indent = writer.indent();

      writer | "static rs::ml::core::DataValue get(const rs::ml::core::Track* track, std::uint16_t fieldId)"
             | "{"
             | "  switch (fieldId)"
             | "  {";
        
      for (auto field : fields)
      {
        auto indent = writer.indent(2);

        if (field->type()->base_type() == ::reflection::String)
        {
          writer ( "case %1%:", field->id() )
                 | "{";
          writer ( "  auto val = track->%1%();", field->name()->c_str() )
                 | "  return val == nullptr ? rs::ml::core::DataValue{} : std::string_view{val->c_str(), val->size()};"
                 | "}";
        }
        else if (flatbuffers::IsInteger(field->type()->base_type()))
        {
          writer ( "case %1%:", field->id() )
                 ( "  return {static_cast<std::int64_t>(track->%1%())};", field->name()->c_str() );
        }
      }

      writer | "    default: "
             | "      return {};"
             | "  }"
             | "}";
    }

    template<typename Fields>
    void generateTrackTAccessor(CppCodeWriter& writer, const Fields& fields)
    {
      auto indent = writer.indent();

      writer | "static rs::ml::core::DataValue get(const rs::ml::core::TrackT& track, std::uint16_t fieldId)"
             | "{"
             | "  switch (fieldId)"
             | "  {";
        
      for (auto field : fields)
      {
        auto indent = writer.indent(2);

        if (field->type()->base_type() == ::reflection::String)
        {
          writer ( "case %1%:", field->id() )
                 ( "  return track.%1%.empty() ? rs::ml::core::DataValue{} : std::string_view{track.%1%};", field->name()->c_str() );
        }
        else if (flatbuffers::IsInteger(field->type()->base_type()))
        {
          writer ( "case %1%:", field->id() )
                 ( "  return {static_cast<std::int64_t>(track.%1%)};", field->name()->c_str() );
        }
      }

      writer | "    default: "
             | "      return {};"
             | "  }"
             | "}";
    }
  };
}

