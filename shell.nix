let
  pin = builtins.fromJSON (builtins.readFile ./nixpkgs.json);
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/${pin.owner}/${pin.repo}/archive/${pin.rev}.tar.gz";
    sha256 = pin.sha256;
  }) { };
  toolchain = builtins.fromJSON (builtins.readFile ./script/ao/toolchain.json);
  requireToolVersion = name: expected: actual:
    if expected == actual then true else throw (
      "Aobus requires ${name} ${expected}, but pinned Nixpkgs resolves ${actual}. "
      + "Follow doc/dev/dependency-upgrades.md to update the toolchain contract and both platform locks."
    );
  lexy-src = (
    builtins.fetchTarball {
      url = "https://github.com/foonathan/lexy/archive/refs/tags/v2025.05.0.tar.gz";
      sha256 = "14j2z7x2l65q95j5br5nw7awgd87p9m2xw7mma4qspiricd0rniq";
    }
  );
  lexy = pkgs.stdenv.mkDerivation {
    pname = "lexy";
    version = "2025.05.0";
    src = lexy-src;
    installPhase = ''
      mkdir -p $out/include
      cp -r include/* $out/include/
    '';
  };
  fakeit-src = (
    builtins.fetchTarball {
      url = "https://github.com/eranpeer/FakeIt/archive/refs/tags/2.5.0.tar.gz";
      sha256 = "10ar4h803gi7c7byp1lm8dxd4brrsw9ph770p1h2zwamlq0hgqai";
    }
  );
  fakeit = pkgs.stdenv.mkDerivation {
    pname = "fakeit";
    version = "2.5.0";
    src = fakeit-src;
    nativeBuildInputs = [ pkgs.cmake ];
    cmakeFlags = [
      "-DENABLE_TESTING=OFF"
    ];
  };
  stb-image-resize2 = pkgs.fetchurl {
    url = "https://raw.githubusercontent.com/nothings/stb/f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31/stb_image_resize2.h";
    hash = "sha256-mhcl47MDOTviMkzzhFbQOo992TMX4VAH3zZUkmUDVLI=";
  };
  aobus-stb = pkgs.runCommand "aobus-stb" { } ''
    mkdir -p $out/include/stb
    cp -r ${pkgs.stb}/include/stb/. $out/include/stb/
    cp ${stb-image-resize2} $out/include/stb/stb_image_resize2.h
  '';
  aobus-spdlog = pkgs.spdlog.overrideAttrs (oldAttrs: {
    cmakeFlags = (oldAttrs.cmakeFlags or []) ++ [
      "-DSPDLOG_USE_STD_FORMAT=ON"
      "-DSPDLOG_FMT_EXTERNAL=OFF"
    ];
  });
  dependencyReport = pkgs.writeText "aobus-nix-dependencies.json" (builtins.toJSON {
    schemaVersion = 1;
    nixpkgsRevision = pin.rev;
    dependencies = {
      boost = { version = pkgs.boost.version; storePath = toString pkgs.boost; };
      ftxui = { version = pkgs.ftxui.version; storePath = toString pkgs.ftxui; };
      spdlog = { version = aobus-spdlog.version; storePath = toString aobus-spdlog; };
      catch2 = { version = pkgs.catch2_3.version; storePath = toString pkgs.catch2_3; };
      cli11 = { version = pkgs.cli11.version; storePath = toString pkgs.cli11; };
      rapidyaml = { version = pkgs.rapidyaml.version; storePath = toString pkgs.rapidyaml; };
      gsl-lite = { version = pkgs.gsl-lite.version; storePath = toString pkgs.gsl-lite; };
      lexy = { version = lexy.version; storePath = toString lexy; };
      fakeit = { version = fakeit.version; storePath = toString fakeit; };
      stb = {
        version = "${pkgs.stb.version}+resize2-f75e8d1";
        storePath = toString aobus-stb;
      };
    };
  });
in
assert toolchain.schemaVersion == 1;
assert requireToolVersion "Python" toolchain.python pkgs.python3.version;
assert requireToolVersion "Ruff" toolchain.ruff pkgs.python3Packages.ruff.version;
assert requireToolVersion "mypy" toolchain.mypy pkgs.python3Packages.mypy.version;
pkgs.mkShell {
  name = "cpp-dev-env";
  AOBUS_NIX_DEPENDENCY_REPORT = dependencyReport;
  buildInputs =
    with pkgs;
    [
      ccache
      btrfs-progs
      bubblewrap
      rsync
      cmake
      gperf
      ripgrep
      pkg-config
      ninja
      mold
      xvfb
      llvmPackages_22.clang
      gcc
      gdb
      python3 # runs the ./ao development portal (script/ao)
      python3Packages.ruff
      python3Packages.mypy
      llvmPackages_22.clang-tools
      llvmPackages_22.llvm.dev
      llvmPackages_22.clang-unwrapped.dev
      boost.dev
      lmdb
      lmdb.dev
      xxhash
      aobus-spdlog
      mimalloc
      cli11
      catch2_3
      gsl-lite
      rapidyaml
      ftxui
      libogg
      libxml2
      sysprof

      (gtk4.overrideAttrs (old: {
        dontStrip = true;
      }))
      gtkmm4
      librsvg
      glib.dev
      gobject-introspection
      adwaita-icon-theme
      gsettings-desktop-schemas

      ffmpeg
      flac
      alac
      fdk_aac.dev
      mpg123
      pipewire
      alsa-lib
      udev
    ]
    ++ [ lexy fakeit aobus-stb ];
  shellHook = ''
    export PATH="$PATH:/run/current-system/sw/bin"
    # Include gtk4 schemas - need both desktop schemas and gtk4 schemas
    export GSETTINGS_SCHEMA_DIR="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.version}/glib-2.0/schemas:${pkgs.gtk4}/share/gsettings-schemas/gtk4-${pkgs.gtk4.version}/glib-2.0/schemas:$GSETTINGS_SCHEMA_DIR"
    # Set header-only dependency include paths
    export CMAKE_INCLUDE_PATH="${lexy}/include:${aobus-stb}/include:$CMAKE_INCLUDE_PATH"
    export CPLUS_INCLUDE_PATH="${lexy}/include:${aobus-stb}/include/stb:${pkgs.gsl-lite}/include:$CPLUS_INCLUDE_PATH"

    # ccache configuration
    export CCACHE_DIR="$PWD/.cache/ccache"
    export CCACHE_BASEDIR="$PWD"
    export CCACHE_MAXSIZE="10G"
    export CCACHE_COMPRESS=1
    export CCACHE_SLOPPINESS="time_macros"

    # Auto-configure git hooks path
    if [ -d .git ]; then
      git config core.hooksPath script/git-hook
    fi

    echo "Using nixpkgs pinned (see nixpkgs.json)"
    echo "Using GCC by default"
  '';
}
