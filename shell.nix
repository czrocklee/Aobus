let
  pin = builtins.fromJSON (builtins.readFile ./nixpkgs.json);
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/${pin.owner}/${pin.repo}/archive/${pin.rev}.tar.gz";
    sha256 = pin.sha256;
  }) { };
  toolchain = builtins.fromJSON (builtins.readFile ./script/ao/toolchain.json);
  compilerCache = builtins.fromJSON (builtins.readFile ./script/ao/compiler-cache.json);
  bazelToolchain = builtins.fromJSON (builtins.readFile ./script/ao/bazel-toolchain.json);
  # ./ao hashes these local inputs and the normalized GTK debug mode to reuse
  # a matching shell without duplicating the stdenv compiler and linker flags.
  portalInputs = [
    ./shell.nix
    ./nixpkgs.json
    ./script/ao/toolchain.json
    ./script/ao/compiler-cache.json
    ./script/ao/bazel-toolchain.json
  ];
  portalFingerprint = builtins.hashString "sha256" (
    pkgs.lib.concatMapStrings (file: builtins.hashFile "sha256" file + "\n") portalInputs
    + "gtk-unstripped=${if useUnstrippedGtk then "1" else "0"}\n"
  );
  # Keep the portal's interpreter and tools independent of ambient Python paths.
  portalPython = pkgs.python3.withPackages (packages: [
    packages.ruff
    packages.mypy
    packages.markdown-it-py
    packages.mdurl
  ]);
  requireToolVersion = name: expected: actual:
    if expected == actual then true else throw (
      "Aobus requires ${name} ${expected}, but pinned Nixpkgs resolves ${actual}. "
      + "Follow doc/development/dependency-upgrade.md to update the toolchain contract and native locks."
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
    # The store copies are read-only, and Nixpkgs stb may already ship the header.
    chmod -R u+w $out/include/stb
    cp ${stb-image-resize2} $out/include/stb/stb_image_resize2.h
  '';
  aobus-spdlog = pkgs.spdlog.overrideAttrs (oldAttrs: {
    cmakeFlags = (oldAttrs.cmakeFlags or []) ++ [
      "-DSPDLOG_USE_STD_FORMAT=ON"
      "-DSPDLOG_FMT_EXTERNAL=OFF"
    ];
  });
  aobus-icu = pkgs.icu78;
  # Nixpkgs dropped alac as unmaintained and vulnerable. Until Aobus owns its
  # ALAC decoder, Linux builds the codec vcpkg ships to macOS and Windows: this
  # autotools fork's codec sources are identical to macosforge/alac c38887c5.
  aobus-alac = pkgs.stdenv.mkDerivation {
    pname = "alac";
    version = "0.0.7-unstable-2026-04-10";
    outputs = [
      "out"
      "dev"
    ];
    src = pkgs.fetchFromGitHub {
      owner = "mikebrady";
      repo = "alac";
      rev = "5d8c5db0dfcadd5872f28e665cf4f4303447352a";
      hash = "sha256-Wb6I5YHGvBVjVgOutICbRKH96odR3ZgmNS6HQedVahk=";
    };
    nativeBuildInputs = [ pkgs.autoreconfHook ];
  };
  # LMDB 1.0 cannot open 0.9 data or lock files, so every platform builds the
  # contracted release; this is the same GitHub archive the vcpkg port verifies.
  aobus-lmdb = pkgs.lmdb.overrideAttrs (finalAttrs: oldAttrs: {
    version = "1.0.1";
    src = pkgs.fetchFromGitHub {
      owner = "LMDB";
      repo = "lmdb";
      rev = "LMDB_${finalAttrs.version}";
      hash = "sha256-O/TQliIgR/DYMFAAh1X9VWmylP9aZfeD7UcNfGHqsU8=";
    };
    # The Nixpkgs patches edit the 0.9 Makefile, and neither change applies to a
    # native Linux build.
    patches = [ ];
    # The inherited hook writes the 0.9 version into lmdb.pc.
    postInstall = ''
      moveToOutput bin "$bin"
      mkdir -p "$dev/lib/pkgconfig"
      cat > "$dev/lib/pkgconfig/lmdb.pc" <<EOF
      Name: lmdb
      Description: ${oldAttrs.meta.description}
      Version: ${finalAttrs.version}

      Cflags: -I$dev/include
      Libs: -L$out/lib -llmdb
      EOF
    '';
  });
  useUnstrippedGtk = builtins.getEnv "AOBUS_NIX_UNSTRIPPED_GTK" == "1";
  aobus-gtk4 =
    if useUnstrippedGtk then
      pkgs.gtk4.overrideAttrs (_: {
        dontStrip = true;
      })
    else
      pkgs.gtk4;

  hardeningFlags = pkgs.stdenv.cc.defaultHardeningFlags;
  dependencyEntry = package: {
    version = package.version;
    storePath = toString package;
    # Extra identities describe declared outputs without adding realization dependencies.
    drvPath = builtins.unsafeDiscardStringContext package.drvPath;
    outputs = pkgs.lib.genAttrs package.outputs (
      name: builtins.unsafeDiscardStringContext (toString (pkgs.lib.getOutput name package))
    );
  };
  dependencyReport = pkgs.writeText "aobus-nix-dependencies.json" (builtins.toJSON {
    schemaVersion = 1;
    nixpkgsRevision = pin.rev;
    ccWrapper = {
      suffixSalt = pkgs.llvmPackages_22.clang.suffixSalt;
      gccSuffixSalt = pkgs.stdenv.cc.suffixSalt;
      hardeningEnable = hardeningFlags;
      tools = {
        clang = "${pkgs.llvmPackages_22.clang}/bin/clang";
        "clang++" = "${pkgs.llvmPackages_22.clang}/bin/clang++";
        ar = "${pkgs.llvmPackages_22.llvm}/bin/llvm-ar";
        strip = "${pkgs.llvmPackages_22.llvm}/bin/llvm-strip";
        objcopy = "${pkgs.llvmPackages_22.llvm}/bin/llvm-objcopy";
      };
      builtinRoots = map toString [
        pkgs.llvmPackages_22.clang-unwrapped.lib
        pkgs.gcc.cc
        pkgs.glibc.dev
      ];
    };
    dependencies = {
      boost = dependencyEntry pkgs.boost;
      fast-float = dependencyEntry aobus-fast-float;
      ftxui = dependencyEntry pkgs.ftxui;
      mimalloc = dependencyEntry pkgs.mimalloc;
      glib = dependencyEntry pkgs.glib;
      glibmm = dependencyEntry pkgs.glibmm_2_68;
      libsigcxx = dependencyEntry pkgs.libsigcxx_3_0;
      gtkmm = dependencyEntry pkgs.gtkmm4;
      # gtkmm4 consumes the normal GTK output, not the optional unstripped shell variant.
      gtk4 = dependencyEntry pkgs.gtk4;
      cairo = dependencyEntry pkgs.cairo;
      cairomm = dependencyEntry pkgs.cairomm_1_16;
      pango = dependencyEntry pkgs.pango;
      pangomm = dependencyEntry pkgs.pangomm_2_48;
      gdk-pixbuf = dependencyEntry pkgs.gdk-pixbuf;
      graphene = dependencyEntry pkgs.graphene;
      harfbuzz = dependencyEntry pkgs.harfbuzz;
      freetype = dependencyEntry pkgs.freetype;
      fontconfig = dependencyEntry pkgs.fontconfig;
      vulkan-headers = dependencyEntry pkgs.vulkan-headers;
      vulkan-loader = dependencyEntry pkgs.vulkan-loader;
      dbus = dependencyEntry pkgs.dbus;
      icu = dependencyEntry aobus-icu;
      lmdb = dependencyEntry aobus-lmdb;
      xxhash = dependencyEntry pkgs.xxhash;
      spdlog = dependencyEntry aobus-spdlog;
      catch2 = dependencyEntry pkgs.catch2_3;
      cli11 = dependencyEntry pkgs.cli11;
      rapidyaml = dependencyEntry pkgs.rapidyaml;
      gsl-lite = dependencyEntry pkgs.gsl-lite;
      lexy = dependencyEntry lexy;
      gperf = dependencyEntry pkgs.gperf;
      alac = dependencyEntry aobus-alac;
      flac = dependencyEntry pkgs.flac;
      fdk-aac = dependencyEntry pkgs.fdk_aac;
      mpg123 = dependencyEntry pkgs.mpg123;
      opus = dependencyEntry pkgs.libopus;
      alsa-lib = dependencyEntry pkgs.alsa-lib;
      pipewire = dependencyEntry pkgs.pipewire;
      udev = dependencyEntry pkgs.udev;
      # Linux GTK test imports and their already selected Xvfb/private-bus tools.
      libX11 = dependencyEntry pkgs.libX11;
      libXi = dependencyEntry pkgs.libXi;
      libXtst = dependencyEntry pkgs.libXtst;
      # xorgproto supplies the XTest/X11/Xi pkg-config Requires metadata.
      xorgproto = dependencyEntry pkgs.xorgproto;
      fakeit = dependencyEntry fakeit;
      stb = dependencyEntry aobus-stb // {
        version = "${pkgs.stb.version}+resize2-2c980bb";
      };
    };
  });
  # The Python lint and documentation toolchain is pinned by script/ao/toolchain.json.
  toolchainMatchesContract =
    requireToolVersion "Python" toolchain.python pkgs.python3.version
      && requireToolVersion "Ruff" toolchain.ruff pkgs.python3Packages.ruff.version
      && requireToolVersion "mypy" toolchain.mypy pkgs.python3Packages.mypy.version
      && requireToolVersion "markdown-it-py" toolchain.markdown-it-py pkgs.python3Packages.markdown-it-py.version
      && requireToolVersion "mdurl" toolchain.mdurl pkgs.python3Packages.mdurl.version;
  compilerCacheMatchesContract =
    requireToolVersion "ccache" compilerCache.local.linuxVersion pkgs.ccache.version;

  # Linux desktop, audio, and development tooling.
  linuxOnlyInputs = with pkgs; [
    xvfb
    libX11
    libXi
    libXtst
    mold
    sysprof
    gdb
    gcc

    aobus-gtk4
    gtkmm4
    glibmm_2_68
    librsvg
    glib.dev
    gobject-introspection
    adwaita-icon-theme
    gsettings-desktop-schemas

    pipewire
    wireplumber
    dbus
    alsa-lib
    udev
  ];

in
assert pkgs.stdenv.hostPlatform.isLinux || throw "Aobus shell.nix supports Linux only; on macOS use ./ao with the native vcpkg profile.";
assert toolchain.schemaVersion == 1;
assert toolchainMatchesContract;
assert compilerCache.schemaVersion == 1;
assert compilerCacheMatchesContract;
assert bazelToolchain.schemaVersion == 1;
assert requireToolVersion "Bazel" bazelToolchain.version pkgs.bazel_9.version;
pkgs.mkShell {
  name = "cpp-dev-env";
  hardeningEnable = hardeningFlags;
  nativeBuildInputs = [ pkgs.bazel_9 ];
  passthru.portalShell = pkgs.bashInteractive;
  AOBUS_NIX_DEPENDENCY_REPORT = dependencyReport;
  AO_NIX_SHELL_FINGERPRINT = portalFingerprint;
  AO_NIX_PORTAL_PYTHON = "${portalPython}/bin/python3";
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
      portalPython # runs ./ao with the pinned Ruff, mypy, and documentation parser packages
      llvmPackages_22.clang-tools
      llvmPackages_22.llvm.dev
      llvmPackages_22.clang-unwrapped.dev
      boost.dev
      aobus-icu
      aobus-icu.dev
      aobus-lmdb
      aobus-lmdb.dev
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
      aobus-alac
      fdk_aac.dev
      mpg123
      libopus
    ]
    ++ [ lexy fakeit aobus-fast-float aobus-stb ]
    ++ linuxOnlyInputs;
  shellHook = ''
    export PATH="$PATH:/run/current-system/sw/bin"
    # Include gtk4 schemas - need both desktop schemas and gtk4 schemas
    export GSETTINGS_SCHEMA_DIR="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.version}/glib-2.0/schemas:${aobus-gtk4}/share/gsettings-schemas/gtk4-${pkgs.gtk4.version}/glib-2.0/schemas:$GSETTINGS_SCHEMA_DIR"
    # Set header-only dependency include paths
    export CMAKE_INCLUDE_PATH="${lexy}/include:${aobus-stb}/include:$CMAKE_INCLUDE_PATH"
    export CPLUS_INCLUDE_PATH="${lexy}/include:${aobus-stb}/include/stb:${pkgs.gsl-lite}/include:$CPLUS_INCLUDE_PATH"

    echo "Using nixpkgs pinned (see nixpkgs.json)" >&2
    echo "Using GCC by default" >&2
  '';
}
