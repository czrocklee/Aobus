let
  # macOS pins its own nixpkgs. The main pin tracks nixos-unstable, which
  # dropped x86_64-darwin after 26.05, so a darwin evaluation of it fails
  # outright. nixpkgs-darwin.json stays on 26.05 and still resolves every
  # governed dependency at the exact contract version.
  isDarwin = builtins.match ".*-darwin" builtins.currentSystem != null;
  pinFile = if isDarwin then ./nixpkgs-darwin.json else ./nixpkgs.json;
  pin = builtins.fromJSON (builtins.readFile pinFile);
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/${pin.owner}/${pin.repo}/archive/${pin.rev}.tar.gz";
    sha256 = pin.sha256;
  }) { };
  toolchain = builtins.fromJSON (builtins.readFile ./script/ao/toolchain.json);
  requireToolVersion = name: expected: actual:
    if expected == actual then true else throw (
      "Aobus requires ${name} ${expected}, but pinned Nixpkgs resolves ${actual}. "
      + "Follow doc/development/dependency-upgrade.md to update the toolchain contract and both platform locks."
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
  fast-float-src = builtins.fetchTarball {
    url = "https://github.com/fastfloat/fast_float/archive/refs/tags/v8.2.10.tar.gz";
    sha256 = "17m6vvhldycs23biph3m2rqfcqljc7n2dhr7wl7p159hfcp07v0c";
  };
  aobus-fast-float = pkgs.stdenv.mkDerivation {
    pname = "fast-float";
    version = "8.2.10";
    src = fast-float-src;
    nativeBuildInputs = [ pkgs.cmake ];
    cmakeFlags = [
      "-DFASTFLOAT_TEST=OFF"
      "-DFASTFLOAT_INSTALL=ON"
    ];
  };
  stb-image-resize2 = pkgs.fetchurl {
    # v2.18 includes the user-data lifetime fix needed by UBSan builds.
    url = "https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image_resize2.h";
    hash = "sha256-Fz5lRjT2zKrZj2A+aG6iEu7B/o6m0qXl6AVu+hCuOIA=";
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
  aobus-icu = pkgs.icu78;

  # libc++ only constrains std::expected's equality operators in C++26 mode, and
  # those constraints are self-referential -- see cmake/CompilerOptions.cmake for
  # the mechanism. Shadowing that one header with the clauses disabled restores
  # the C++23 form, which is also what libstdc++ compiles against on Linux.
  #
  # Only this header is copied. Duplicating the whole include tree does not work:
  # libc++'s own <cstdint> reaches the SDK through #include_next, and a second
  # copy of the tree on the search path captures that hop, leaving intmax_t
  # unresolved.
  #
  # The gate count is asserted rather than assumed, so a libc++ upgrade that
  # reworks the file fails the build instead of silently producing an unpatched
  # header -- which is also the signal to drop this once upstream constrains the
  # operator non-recursively.
  #
  # Retirement condition: doc/development/macos-portability.md.
  aobus-libcxx-expected = pkgs.runCommand "aobus-libcxx-expected-shim" { } ''
    source="${pkgs.llvmPackages_22.stdenv.cc.libcxx}/include/c++/v1/__expected/expected.h"
    gates=$(grep -c '^#  if _LIBCPP_STD_VER >= 26$' "$source" || true)
    if [ "$gates" != "5" ]; then
      echo "expected 5 C++26 gates in expected.h, found $gates -- libc++ changed," >&2
      echo "re-check whether this shim is still needed." >&2
      exit 1
    fi

    mkdir -p $out/__expected
    sed 's|^#  if _LIBCPP_STD_VER >= 26$|#  if 0 // Aobus: self-referential constraint|' \
      "$source" > $out/__expected/expected.h
  '';

  dependencyReport = pkgs.writeText "aobus-nix-dependencies.json" (builtins.toJSON {
    schemaVersion = 1;
    nixpkgsRevision = pin.rev;
    dependencies = {
      boost = { version = pkgs.boost.version; storePath = toString pkgs.boost; };
      fast-float = { version = aobus-fast-float.version; storePath = toString aobus-fast-float; };
      ftxui = { version = pkgs.ftxui.version; storePath = toString pkgs.ftxui; };
      icu = { version = aobus-icu.version; storePath = toString aobus-icu; };
      spdlog = { version = aobus-spdlog.version; storePath = toString aobus-spdlog; };
      catch2 = { version = pkgs.catch2_3.version; storePath = toString pkgs.catch2_3; };
      cli11 = { version = pkgs.cli11.version; storePath = toString pkgs.cli11; };
      rapidyaml = { version = pkgs.rapidyaml.version; storePath = toString pkgs.rapidyaml; };
      gsl-lite = { version = pkgs.gsl-lite.version; storePath = toString pkgs.gsl-lite; };
      lexy = { version = lexy.version; storePath = toString lexy; };
      fakeit = { version = fakeit.version; storePath = toString fakeit; };
      stb = {
        version = "${pkgs.stb.version}+resize2-2c980bb";
        storePath = toString aobus-stb;
      };
    };
  });
  # The Python lint toolchain is pinned by script/ao/toolchain.json. The darwin
  # pin is nixpkgs 26.05 and cannot carry those versions, so asserting there
  # would make the macOS shell unenterable. Linux and Windows still gate on it,
  # and they are where the Python lint suites run.
  toolchainMatchesContract =
    isDarwin
    || (requireToolVersion "Python" toolchain.python pkgs.python3.version
      && requireToolVersion "Ruff" toolchain.ruff pkgs.python3Packages.ruff.version
      && requireToolVersion "mypy" toolchain.mypy pkgs.python3Packages.mypy.version);

  # Linux desktop, Linux audio, and Linux-only development tooling. macOS builds
  # no GTK frontend and no native audio backend yet, so none of this applies
  # there; several of these packages do not exist on darwin at all.
  linuxOnlyInputs = with pkgs; [
    xvfb
    mold
    sysprof
    gdb
    gcc

    (gtk4.overrideAttrs (old: {
      dontStrip = true;
    }))
    gtkmm4
    librsvg
    glib.dev
    gobject-introspection
    adwaita-icon-theme
    gsettings-desktop-schemas

    pipewire
    alsa-lib
    udev
  ];

  darwinOnlyInputs = with pkgs; [
    llvmPackages_22.lldb
  ];

  # macOS needs the LLVM 22 stdenv explicitly: the default darwin stdenv is
  # clang 21, and CMAKE_CXX_STANDARD 26 wants the same compiler Linux uses.
  makeShell = if isDarwin then pkgs.mkShell.override { stdenv = pkgs.llvmPackages_22.stdenv; } else pkgs.mkShell;
in
assert toolchain.schemaVersion == 1;
assert toolchainMatchesContract;
makeShell {
  name = "cpp-dev-env";
  AOBUS_NIX_DEPENDENCY_REPORT = dependencyReport;
  buildInputs =
    with pkgs;
    [
      ccache
      rsync
      cmake
      git
      gperf
      ripgrep
      pkg-config
      ninja
      llvmPackages_22.clang
      python3 # runs the ./ao development portal (script/ao)
      python3Packages.ruff
      python3Packages.mypy
      llvmPackages_22.clang-tools
      llvmPackages_22.llvm.dev
      llvmPackages_22.clang-unwrapped.dev
      boost.dev
      aobus-icu
      aobus-icu.dev
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
      libxml2

      ffmpeg
      flac
      alac
      fdk_aac.dev
      mpg123
      libopus
    ]
    ++ [ lexy fakeit aobus-fast-float aobus-stb ]
    ++ (if isDarwin then darwinOnlyInputs else linuxOnlyInputs);
  shellHook = ''
    export PATH="$PATH:/run/current-system/sw/bin"
    ${pkgs.lib.optionalString (!isDarwin) ''
      # Include gtk4 schemas - need both desktop schemas and gtk4 schemas
      export GSETTINGS_SCHEMA_DIR="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.version}/glib-2.0/schemas:${pkgs.gtk4}/share/gsettings-schemas/gtk4-${pkgs.gtk4.version}/glib-2.0/schemas:$GSETTINGS_SCHEMA_DIR"
    ''}
    # Set header-only dependency include paths
    export CMAKE_INCLUDE_PATH="${lexy}/include:${aobus-stb}/include:$CMAKE_INCLUDE_PATH"
    export CPLUS_INCLUDE_PATH="${lexy}/include:${aobus-stb}/include/stb:${pkgs.gsl-lite}/include:$CPLUS_INCLUDE_PATH"

    # ccache configuration
    export CCACHE_DIR="$PWD/.cache/ccache"
    export CCACHE_BASEDIR="$PWD"
    ${pkgs.lib.optionalString isDarwin ''
      # The macOS checkout is normally an SMB view of the authoritative Linux
      # tree, so generated state must not sit beside the source -- the same
      # rule the Windows portal enforces with %LOCALAPPDATA%. Keep the compiler
      # cache on the local disk.
      export CCACHE_DIR="''${AOBUS_STATE_ROOT:-$HOME/Library/Caches/Aobus}/ccache"

      # Consumed by cmake/CompilerOptions.cmake, which prepends it with -isystem.
      export AOBUS_LIBCXX_EXPECTED_SHIM="${aobus-libcxx-expected}"
    ''}
    export CCACHE_MAXSIZE="10G"
    export CCACHE_COMPRESS=1
    export CCACHE_SLOPPINESS="time_macros"

    # Auto-configure git hooks path
    if [ -d .git ]; then
      git config core.hooksPath script/git-hook
    fi

    echo "Using nixpkgs pinned (see ${builtins.baseNameOf pinFile})"
    echo "Using ${if isDarwin then "Clang 22 (LLVM stdenv)" else "GCC"} by default"
  '';
}
