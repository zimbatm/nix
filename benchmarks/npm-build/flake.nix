{
  description = "Benchmark: in-process builtin:fetchurl vs fork+sandbox";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};

      # 197 npm tarball URLs with integrity hashes (from jsonld-cli v2.0.0).
      urls = builtins.fromJSON (builtins.readFile ./urls.json);

      # Individual FODs: one builtin:fetchurl per npm tarball.
      individualFetches = map (
        entry:
        derivation {
          name = builtins.baseNameOf entry.url;
          builder = "builtin:fetchurl";
          inherit system;
          url = entry.url;
          outputHashMode = "flat";
          outputHashAlgo = "sha512";
          outputHash = entry.integrity;
          preferLocalBuild = true;
          allowSubstitutes = false;
        }
      ) urls;

      allIndividual = pkgs.runCommand "all-individual" { } (
        ''
          mkdir -p $out
        ''
        + builtins.concatStringsSep "\n" (
          builtins.genList (
            i:
            let
              drv = builtins.elemAt individualFetches i;
            in
            "ln -s ${drv} $out/${toString i}.tgz"
          ) (builtins.length individualFetches)
        )
      );

      # Same as individualFetches but allows substituters (to measure lookup overhead).
      individualWithSubst = map (
        entry:
        derivation {
          name = builtins.baseNameOf entry.url;
          builder = "builtin:fetchurl";
          inherit system;
          url = entry.url;
          outputHashMode = "flat";
          outputHashAlgo = "sha512";
          outputHash = entry.integrity;
          preferLocalBuild = true;
        }
      ) urls;

      allIndividualWithSubst = pkgs.runCommand "all-individual-with-subst" { } (
        ''
          mkdir -p $out
        ''
        + builtins.concatStringsSep "\n" (
          builtins.genList (
            i:
            let
              drv = builtins.elemAt individualWithSubst i;
            in
            "ln -s ${drv} $out/${toString i}.tgz"
          ) (builtins.length individualWithSubst)
        )
      );

      # Individual FODs using pkgs.fetchurl (shell-based, forks curl in each sandbox).
      pkgsFetches = map (
        entry:
        (pkgs.fetchurl {
          url = entry.url;
          hash = entry.integrity;
        }).overrideAttrs
          {
            allowSubstitutes = false;
          }
      ) urls;

      allPkgsFetchurl = pkgs.runCommand "all-pkgs-fetchurl" { } (
        ''
          mkdir -p $out
        ''
        + builtins.concatStringsSep "\n" (
          builtins.genList (
            i:
            let
              drv = builtins.elemAt pkgsFetches i;
            in
            "ln -s ${drv} $out/${toString i}.tgz"
          ) (builtins.length pkgsFetches)
        )
      );
    in
    {
      packages.${system} = {
        # 197 individual builtin:fetchurl FODs (one sandbox per tarball)
        individual = allIndividual;
        # Same but with substituters enabled (measures cache lookup overhead)
        individual-with-subst = allIndividualWithSubst;
        # 197 individual pkgs.fetchurl FODs (shell+curl in each sandbox)
        pkgs-fetchurl = allPkgsFetchurl;
        # Minimal target that only pulls in stdenv closure (no FODs).
        # Used by bench.sh to pre-populate isolated stores.
        stdenv-warmup = pkgs.runCommand "stdenv-warmup" { } "echo ok > $out";
      };
    };
}
