-- premake5.lua

project "ml"
  kind "SharedLib"
  dependson "gen"
  files {"ml/**.C", "ml/**.capnp.cpp"} 
  includedirs {"../../include", "../../include/rs/ml/core", "../../3rd"}
  warnings "Extra"
	buildoptions {'-fPIC', '--std=c++1z'} 
