-- premake5.lua

project "rml"
  kind "SharedLib"
  files {"src/**.C", "src/**.capnp.cpp"} 
  includedirs {"include", "include/rml"}
	buildoptions {'-fPIC', '--std=c++1z'} 
