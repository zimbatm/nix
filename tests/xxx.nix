{ ... }@args:
let
  pkgs = import ./nix-default.nix args;
in
  pkgs.foobar
