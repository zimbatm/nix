#!/usr/bin/env bash
#
# Compare nixpkgs' text-writing builders before and after patching them to
# use builtin:writeFiles (see writefiles-overlay.nix):
#
#   stock     pkgs.writeText            full stdenvNoCC shell build
#   patched   pkgs.writeText (overlay)  slim builtin:writeFiles build
#
# Both build N freshly-nonce'd `writeText` outputs per invocation, through
# the real nixpkgs `writeTextFile` machinery, against a throwaway chroot
# store using the local dev nix. The stock path's stdenv closure is seeded
# into the chroot once (untimed); substituters are off during timing so we
# measure local build cost only. nixpkgs evaluation cost is paid by both
# and cancels out — the difference is the builder.
#
# Usage:
#   nix develop --command ./benchmarks/writefiles/nixpkgs-bench.sh
#   N=200 RUNS=10 ./benchmarks/writefiles/nixpkgs-bench.sh
#
# Internal: ./nixpkgs-bench.sh _run <stock|patched>

set -euo pipefail

HERE=$(dirname "$(readlink -f "$0")")
ROOT=$(readlink -f "$HERE/../..")

default_builddir=$ROOT/build-wf
[[ -x $ROOT/build-opt/src/nix/nix ]] && default_builddir=$ROOT/build-opt
NIX=${NIX:-$default_builddir/src/nix/nix}
N=${N:-100}
SIZE=${SIZE:-256}
OVERLAY=$HERE/writefiles-overlay.nix

export NIX_REMOTE=
FEATURES="nix-command flakes ca-derivations"

nixc() {
    "$NIX" --extra-experimental-features "$FEATURES" --store "$CHROOT" \
        --option sandbox false --option substitute false --option substituters '' "$@"
}

pkgsExpr() { # $1 = overlay list contents
    echo "import <nixpkgs> { config = {}; overlays = [ $1 ]; }"
}

buildN() { # $1 = pkgs expr, $2 = nonce, $3 = pad
    nixc build --no-link --print-out-paths --impure --expr "
      let pkgs = $1;
      in builtins.genList
        (i: pkgs.writeText \"wt-$2-\${toString i}\" \"content-$2-\${toString i}-$3\")
        $N" >/dev/null
}

if [[ "${1:-}" == "_run" ]]; then
    nonce=$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')
    pad=$(head -c "$SIZE" /dev/zero | tr '\0' x)
    case "$2" in
        stock) buildN "$(pkgsExpr '')" "$nonce" "$pad" ;;
        patched) buildN "$(pkgsExpr "(import $OVERLAY)")" "$nonce" "$pad" ;;
    esac
    exit 0
fi

# ---- orchestrator ------------------------------------------------------

CHROOT=$(mktemp -d)/store
export CHROOT
mkdir -p "$CHROOT"

echo "nix       : $NIX"
echo "chroot    : $CHROOT"
echo "files/run : $N    (~$SIZE bytes each)"
echo

echo "Seeding stock stdenv closure into the chroot store (with substituters)..."
"$NIX" --extra-experimental-features "$FEATURES" --store "$CHROOT" --option sandbox false \
    build --no-link --impure \
    --expr "($(pkgsExpr '')).writeText \"seed\" \"seed\"" >/dev/null
echo "  chroot now holds $(ls "$CHROOT/nix/store" | wc -l) paths"
echo

echo "Running benchmark (via hyperfine)..."
exec "$NIX" --extra-experimental-features "$FEATURES" run nixpkgs#hyperfine -- \
    --warmup "${WARMUP:-1}" --runs "${RUNS:-8}" \
    --command-name "stock   (shell build)" "$HERE/nixpkgs-bench.sh _run stock" \
    --command-name "patched (writeFiles) " "$HERE/nixpkgs-bench.sh _run patched"
