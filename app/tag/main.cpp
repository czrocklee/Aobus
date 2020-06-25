
#include <rs/tag/mp4/File.h>
#include <iostream>
#include <QtCore/QVariant>

int main()
{
  rs::tag::mp4::File file{"/home/rocklee/3rd/123.m4a", rs::tag::File::Mode::ReadOnly};
  auto meta = file.loadMetadata();
  std::cout << std::get<std::uint64_t>(meta.get(rs::tag::MetaField::TrackNumber)) << std::endl
            << std::get<std::string>(meta.get(rs::tag::MetaField::Artist)) << std::endl
              << std::get<std::string>(meta.get(rs::tag::MetaField::Title)) << std::endl;
        
  return 0;
}
