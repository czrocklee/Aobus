from conans.model import Generator
from conans import ConanFile
from pprint import pprint
import os
from glob import glob
from pathlib import Path

class Bazel(Generator):
    @property
    def filename(self):
        return "conanbuildinfo.bzl"

    @property
    def content(self):
        output = self.__gen_root_path_vars()
        output += "\ndef conan_basic_setup():"
        output += "".join([self.__gen_local_repo(dep_name, dep_cpp_info) for dep_name, dep_cpp_info in self.deps_build_info.dependencies])
        return output
 
    def __gen_root_path_vars(self):
        output = ""
        for dep_name, dep_cpp_info in self.deps_build_info.dependencies:
            output += f"""CONAN_{dep_name.upper().replace("-", "_")}_ROOT = "{dep_cpp_info.cpp_info.rootpath}"\n"""
        return output

    def __gen_local_repo(self, dep_name, dep_cpp_info):
        output = f"""
    if "conan_{dep_name}" not in native.existing_rules():
        native.new_local_repository(
            name = "conan_{dep_name.replace("-", "_")}",
            path = "{dep_cpp_info.cpp_info.rootpath}",
            build_file_content =
            \"\"\"{self.__gen_build_file_content(dep_cpp_info)}
\"\"\",
    )
"""
        return output

    def __gen_build_file_content(self, dep_cpp_info):
        pprint(vars(dep_cpp_info))
        root_dir = dep_cpp_info.cpp_info.rootpath
        output = f"""
cc_library(
    name = "headers",
    visibility = ["//visibility:public"],
"""
        if dep_cpp_info.include_paths:
            output += f"""    hdrs = glob({[os.path.join(os.path.relpath(p, root_dir), "**/*") for p in dep_cpp_info.include_paths]}),\n"""
            output += f"""    includes = {[os.path.relpath(p, root_dir) for p in dep_cpp_info.include_paths]},\n"""
        if dep_cpp_info.defines:
            output += f"    defines = {dep_cpp_info.defines}\n"
        if dep_cpp_info.cxxflags:
            output += f"    copts = {dep_cpp_info.cxxflags}\n"
        if dep_cpp_info.exelinkflags:
            output += f"    linkopts = {dep_cpp_info.exelinkflags}\n"
        output += ")\n"

        output += self.__gen_binary_targets(root_dir, dep_cpp_info.bin_paths)

        for lib in dep_cpp_info.libs:
            cc_import = self.__gen_cc_import_target(root_dir, lib, dep_cpp_info.lib_paths)
            if cc_import:
                output += cc_import
                output += self.__gen_cc_library_target(root_dir, lib)
        return output

    def __glob_multiple(self, paths, pattern):
        return [item for p in paths for item in glob(os.path.join(p, pattern))]

    def __gen_cc_import_target(self, root_dir, lib, lib_paths):
        output = ""
        lib_file_a = self.__glob_multiple(lib_paths, f"lib{lib}.a")
        lib_file_so = self.__glob_multiple(lib_paths, f"lib{lib}.so")
        if not lib_file_a and not lib_file_so:
            return
        output += f"""
cc_import(
    name = "{lib}_lib",
    visibility = ["//visibility:private"],
"""
        if lib_file_a:
            output += f"""    static_library = "{os.path.relpath(lib_file_a[0], root_dir)}",\n"""
        if lib_file_so:
            output += f"""    shared_library = "{os.path.relpath(lib_file_so[0], root_dir)}",\n"""
        output += ")\n"
        return output

    def __gen_cc_library_target(self, root_dir, lib):
        return f"""
cc_library(
    name = "{lib}",
    deps = [
        ":headers",
        ":{lib}_lib"
    ],
    visibility = ["//visibility:public"],
)
"""
    def __gen_binary_targets(self, root_dir, bin_paths):
        output = ""
        for binary in self.__glob_multiple(bin_paths, "*"):
            if os.access(binary, os.X_OK):
                output += f"""
sh_binary(
    name = "{Path(binary).stem}",
    srcs = ["{os.path.relpath(binary, root_dir)}"],
    visibility = ["//visibility:public"],
)
"""
        return output

class MyCustomGeneratorPackage(ConanFile):
    name = "BazelGen"
    version = "0.1"
    url = "https://github.com/memsharded/conan-premake"
    license = "MIT"
