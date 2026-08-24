# macOS ships bash 3.2.57 and never updates it, while nixpkgs' stdenv setup
# requires bash 5. nix-shell takes its interactive shell from PATH, so the ./ao
# portal resolves this derivation and exports it as NIX_BUILD_SHELL before
# entering shell.nix.
#
# Kept separate from shell.nix so the bootstrap shell resolves without first
# evaluating the full development environment, while still using the same pin.
let
  pin = builtins.fromJSON (builtins.readFile ./nixpkgs-darwin.json);
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/${pin.owner}/${pin.repo}/archive/${pin.rev}.tar.gz";
    sha256 = pin.sha256;
  }) { };
in
pkgs.bashInteractive
