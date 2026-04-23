{
  pkgs ? import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/refs/heads/nixos-unstable.tar.gz";
  }) { },
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
in
pkgs.mkShell {
  name = "cpp-dev-env";
  buildInputs =
    with pkgs;
    [
      cmake
      gperf
      ripgrep
      pkg-config
      ninja
      clang
      gcc
      gdb
      clang-tools
      boost.dev
      lmdb
      lmdb.dev
      spdlog
      mimalloc
      catch2
      gsl-lite

      (gtk4.overrideAttrs (old: {
        dontStrip = true;
      }))
      gtkmm4
      glib.dev
      gobject-introspection
      adwaita-icon-theme
      gsettings-desktop-schemas

      ffmpeg
      pipewire
      alsa-lib
    ]
    ++ [ lexy ];
  shellHook = ''
    export PATH="$PATH:/run/current-system/sw/bin"
    # Include gtk4 schemas - need both desktop schemas and gtk4 schemas
    export GSETTINGS_SCHEMA_DIR="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.version}/glib-2.0/schemas:${pkgs.gtk4}/share/gsettings-schemas/gtk4-${pkgs.gtk4.version}/glib-2.0/schemas:$GSETTINGS_SCHEMA_DIR"
    # Set Lexy include path
    export CPLUS_INCLUDE_PATH="${lexy}/include:${pkgs.gsl-lite}/include:$CPLUS_INCLUDE_PATH"
    echo "Using nixpkgs pinned to nixos-unstable"
    echo "Using GCC by default"
  '';
}
