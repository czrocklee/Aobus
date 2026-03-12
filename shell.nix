{
  pkgs ?
    import (builtins.fetchTarball {
      # Pinning to the NixOS 25.11 stable channel.
      url = "https://github.com/NixOS/nixpkgs/archive/refs/heads/nixos-25.11.tar.gz";
    }) {},
}:
pkgs.mkShell {
  name = "cpp-dev-env";
  #pkgs.boost.override { enableShared = false; enabledStatic = true; }
  buildInputs = with pkgs; [
    cmake
    pkg-config
    ninja
    gcc
    gdb
    clang-tools
    boost.dev
    lmdb
    lmdb.dev
    flatbuffers
    mimalloc

    kdePackages.qtbase
    kdePackages.qtwayland
    kdePackages.qtstyleplugin-kvantum
  ];
  shellHook = ''
    export QT_QPA_PLATFORM=xcb
    echo "Using nixpkgs pinned to NixOS 25.11 (stable release)"
  '';
}
