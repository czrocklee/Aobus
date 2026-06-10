{
  pkgs ? import (
    let pin = builtins.fromJSON (builtins.readFile ./nixpkgs.json);
    in builtins.fetchTarball {
      url = "https://github.com/${pin.owner}/${pin.repo}/archive/${pin.rev}.tar.gz";
      sha256 = pin.sha256;
    }
  ) { },
}:
let
  lexy-src = (
    builtins.fetchTarball {
      url = "https://github.com/foonathan/lexy/archive/refs/tags/v2025.05.0.tar.gz";
      sha256 = "14j2z7x2l65q95j5br5nw7awgd87p9m2xw7mma4qspiricd0rniq";
    }
  );
  lexy = pkgs.stdenv.mkDerivation {
    name = "lexy";
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
    name = "fakeit";
    src = fakeit-src;
    nativeBuildInputs = [ pkgs.cmake ];
    cmakeFlags = [
      "-DENABLE_TESTING=OFF"
    ];
  };
in
pkgs.mkShell {
  name = "cpp-dev-env";
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
      clang
      gcc
      gdb
      clang-tools
      llvmPackages.llvm.dev
      llvmPackages.clang-unwrapped.dev
      boost.dev
      lmdb
      lmdb.dev
      (spdlog.overrideAttrs (oldAttrs: {
        cmakeFlags = (oldAttrs.cmakeFlags or []) ++ [
          "-DSPDLOG_USE_STD_FORMAT=ON"
          "-DSPDLOG_FMT_EXTERNAL=OFF"
        ];
      }))
      mimalloc
      cli11
      catch2_3
      gsl-lite
      rapidyaml
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
      mpg123
      pipewire
      alsa-lib
      udev
      xvfb-run
    ]
    ++ [ lexy fakeit ];
  shellHook = ''
    export PATH="$PATH:/run/current-system/sw/bin"
    # Include gtk4 schemas - need both desktop schemas and gtk4 schemas
    export GSETTINGS_SCHEMA_DIR="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.version}/glib-2.0/schemas:${pkgs.gtk4}/share/gsettings-schemas/gtk4-${pkgs.gtk4.version}/glib-2.0/schemas:$GSETTINGS_SCHEMA_DIR"
    # Set header-only dependency include paths
    export CPLUS_INCLUDE_PATH="${lexy}/include:${pkgs.gsl-lite}/include:$CPLUS_INCLUDE_PATH"

    # ccache configuration
    export CCACHE_DIR="$PWD/.cache/ccache"
    export CCACHE_BASEDIR="$PWD"
    export CCACHE_MAXSIZE="10G"
    export CCACHE_COMPRESS=1
    export CCACHE_SLOPPINESS="time_macros"

    echo "Using nixpkgs pinned (see nixpkgs.json)"
    echo "Using GCC by default"
  '';
}
