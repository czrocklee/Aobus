{
  pkgs ?
    import (builtins.fetchTarball {
      url = "https://github.com/NixOS/nixpkgs/archive/refs/heads/nixos-unstable.tar.gz";
    }) {},
}:
pkgs.mkShell {
  name = "cpp-dev-env";
  buildInputs = with pkgs; [
    cmake
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
    flatbuffers
    mimalloc
    catch2

    (gtk4.overrideAttrs (old: {
      dontStrip = true;
    }))
    gtkmm4
    glib.dev
    gobject-introspection
    adwaita-icon-theme
    gsettings-desktop-schemas

    kdePackages.qtbase
    ffmpeg
  ];
  shellHook = ''
    export PATH="$PATH:/run/current-system/sw/bin"
    # Include gtk4 schemas - need both desktop schemas and gtk4 schemas
    export GSETTINGS_SCHEMA_DIR="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.version}/glib-2.0/schemas:${pkgs.gtk4}/share/gsettings-schemas/gtk4-${pkgs.gtk4.version}/glib-2.0/schemas:$GSETTINGS_SCHEMA_DIR"
    echo "Using nixpkgs pinned to nixos-unstable"
    echo "Using GCC by default"
  '';
}
