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

#include <lib/rs/ml/query/TrackFieldAccessorGenerator.H>

#include <flatbuffers/reflection.h>
#include <flatbuffers/util.h>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>

namespace
{
  using Fields = std::vector<const ::reflection::Field*>;
  using Generator = std::function<void(std::ostream&, const Fields&)>;

  std::vector<std::pair<std::string, Generator>> generators = 
  {
    {"lib/rs/ml/query/gen/TrackFieldAccessor.H", rs::ml::query::TrackFieldAccessorGenerator{}}
  };

  namespace bpo = boost::program_options;
}

int main(int argc, char* argv[])
{
  std::string schemaFile;

  bpo::options_description desc("Allowed options");
  desc.add_options()
    ("help", "produce help message")
    ("schema-file", bpo::value<std::string>(&schemaFile)->required(), "binary schema-file generated from Track.fbs");

  bpo::positional_options_description p;
  p.add("schema-file", -1);

  bpo::variables_map vm;
  bpo::store(bpo::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
  bpo::notify(vm);
      
  if (vm.count("help"))
  {
    std::cout << desc << std::endl;
    return 0;
  }

  std::ostringstream oss;
	std::ifstream ifs(schemaFile.c_str(), std::ios::binary);
  oss << ifs.rdbuf();
  std::string schemaContent = oss.str();

  flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t *>(schemaContent.c_str()), schemaContent.length());

  if (!reflection::VerifySchemaBuffer(verifier))
  {
    std::cerr << "Invalid binary schema file." << std::endl;
    return 1;
  }

  // Make sure the schema is what we expect it to be.
  auto &schema = *reflection::GetSchema(schemaContent.c_str());
  auto root_table = schema.root_table();
  
  if (std::strcmp(root_table->name()->c_str(), "rs.ml.core.Track") != 0)
  {
    std::cerr << "Invalid root table, 'Track' expceted." << std::endl;
    return 1;
  }

	Fields fields{root_table->fields()->begin(), root_table->fields()->end()};

  std::sort(fields.begin(), fields.end(), [](auto l, auto r) { return l->id() < r->id(); });

  for (auto& pair : generators)
  {
    auto dir = boost::filesystem::path{pair.first}.parent_path();
    
    if(!boost::filesystem::exists(dir))
    {
      if (!boost::filesystem::create_directory(dir))
      {
        std::cerr << "Failed to create directory : " << dir << std::endl;
        return 1;
      }
    }

		std::ofstream ofs(pair.first.c_str(), std::ios::out | std::ios::trunc);

    if(!ofs)
    {  
      std::cerr << "File opening failed : " << pair.first << std::endl;;
      return 1;
    }

		pair.second(ofs, fields);
  }

  return 0;
  /*
  auto hp_field_ptr = fields->LookupByKey("hp");
  TEST_NOTNULL(hp_field_ptr);
  auto &hp_field = *hp_field_ptr;
  TEST_EQ_STR(hp_field.name()->c_str(), "hp");
  TEST_EQ(hp_field.id(), 2);
  TEST_EQ(hp_field.type()->base_type(), reflection::Short);
  auto friendly_field_ptr = fields->LookupByKey("friendly");
  TEST_NOTNULL(friendly_field_ptr);
  TEST_NOTNULL(friendly_field_ptr->attributes());
  TEST_NOTNULL(friendly_field_ptr->attributes()->LookupByKey("priority"));*/
}

