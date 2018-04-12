-- premake5.lua

project "rml"
  kind "ConsoleApp"
  language "C++"
  includedirs {"../include", "../3rd"}
  files "rml/*.C"
  warnings "Extra"
  buildoptions {'--std=c++1z'} 
  linkoptions {'-no-pie'}
  links {'ml', 'capnp', 'kj', 'lmdb', 'tag', 'crypto', 'boost_program_options', 'boost_system', 'boost_filesystem'}

package.path = package.path .. ";../3rd/premake-qt/?.lua"
require ("qt")
local qt = premake.extensions.qt


project "rmlGui"
  kind "ConsoleApp"
  language "C++"
  
  qt.enable()
  qtmodules {"core", "gui", "widgets", "concurrent"}
  qtlibpath "/usr/lib/x86_64-linux-gnu/"
  qtbinpath "/usr/lib/x86_64-linux-gnu/qt5/bin"
  qtincludepath "/usr/include/x86_64-linux-gnu/qt5"
  qtprefix "Qt5"

  includedirs {"rmlGui", "../include", "../3rd"}
  files {"rmlGui/*.H", "rmlGui/*.C", "rmlGui/*.ui", "rmlGui/*.qrc"}
  warnings "Extra"
  buildoptions {'-fPIC', '--std=c++1z'}
  linkoptions {'-no-pie', '-Wl,--no-as-needed'}
  links {'ml', 'capnp', 'kj', 'lmdb', 'tag', 'crypto', 'boost_program_options', 'boost_system', 'boost_filesystem'}






