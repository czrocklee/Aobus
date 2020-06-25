
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <array>
#include <string>
#include <iostream>
#include <boost/endian/conversion.hpp>
#include <map>
#include "src/rs/tag/mp4/Atom.h"

void print(const rs::tag::mp4::Atom& node, int levels)
{
  std::cerr << std::string(levels * 2, '_') << node.type() << std::endl;
  node.visitChildren([&](const auto& node) {
    print(node, levels + 1);
    return true;
  });
}

int main()
{
  namespace bpi = boost::interprocess;
  bpi::file_mapping file("/home/rocklee/abc.m4a", bpi::read_only);
  bpi::mapped_region region(file, bpi::read_only);

  auto node = rs::tag::mp4::fromBuffer(region.get_address(), region.get_size());
  print(node, 0);

  // node.inorder([](const auto& atom) { std::cout << atom.type() << std::endl; });

  return 0;
}
