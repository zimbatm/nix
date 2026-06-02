#!/usr/bin/env bash
#
# Benchmark four ways to materialise file content into the Nix store, to
# situate the new `builtin:writeFiles` builder:
#
#   toFile      builtins.toFile            eval-time, no derivation, no build
#   addToStore  nix store add              copy an existing on-disk file in
#   writeFiles  builtin:writeFiles         inline build, no fork, no sandbox
#   shellWrite  a writeText-style drv      full build that forks a shell
#
# `shellWrite` stands in for nixpkgs `pkgs.writeText` / `writeTextFile`:
# a normal derivation whose builder forks bash to write the file. We use
# a hand-rolled one (rather than nixpkgs writeText) so the measurement is
# the build/fork overhead, not the realisation of stdenv's closure.
#
# Everything runs against an isolated chroot store under $TMPDIR using the
# freshly built ./build-wf nix, so the real store is never touched. Each
# timed run uses fresh (nonce'd) content, so we measure real work rather
# than cache hits.
#
# Usage:
#   ./bench.sh                 # seed + run hyperfine, default N=100 SIZE=64
#   N=500 SIZE=1024 ./bench.sh
#   NIX=/path/to/nix ./bench.sh
#
# Internal: ./bench.sh _run <method>   (one timed iteration; used by hyperfine)

set -euo pipefail

HERE=$(dirname "$(readlink -f "$0")")
ROOT=$(readlink -f "$HERE/../..")

# Prefer an optimized build (build-opt) over the -O0 dev build (build-wf):
# the debug build is ~2.5x slower and not representative.
default_builddir=$ROOT/build-wf
[[ -x $ROOT/build-opt/src/nix/nix ]] && default_builddir=$ROOT/build-opt
NIX=${NIX:-$default_builddir/src/nix/nix}
NIXSTORE=${NIXSTORE:-$default_builddir/src/nix/nix-store}
N=${N:-100}
SIZE=${SIZE:-64}

# Isolated chroot store + state, so we never write to the real store and
# the dev-build nix (which knows builtin:writeFiles) does the work itself.
export NIX_REMOTE=
FEATURES="nix-command flakes ca-derivations"

system=${system:-$("$NIX" --extra-experimental-features "$FEATURES" eval --impure --raw --expr 'builtins.currentSystem' 2>/dev/null)}
export system

# Padding to reach ~SIZE bytes; the nonce+index prefix keeps each file unique.
pad() { head -c "$SIZE" /dev/zero | tr '\0' x; }

# --substitute false: measure local work only, no binary-cache round-trips.
nixc() {
    "$NIX" --extra-experimental-features "$FEATURES" --store "$CHROOT" \
        --option sandbox false --option substitute false --option substituters '' "$@"
}

run_toFile() {
    local nonce=$1 PAD=$2
    nixc eval --raw --expr "
      builtins.toString (builtins.genList
        (i: builtins.toFile \"tf-$nonce-\${toString i}\" \"$nonce-\${toString i}-$PAD\")
        $N)" >/dev/null
}

run_addToStore() {
    local nonce=$1 PAD=$2
    local d
    d=$(mktemp -d)
    local i
    for ((i = 0; i < N; i++)); do printf '%s' "$nonce-$i-$PAD" >"$d/f$i"; done
    # `nix store add` is single-path; the legacy nix-store --add adds a batch.
    # shellcheck disable=SC2046
    "$NIXSTORE" --store "$CHROOT" --add $(printf '%s ' "$d"/f*) >/dev/null
    rm -rf "$d"
}

run_writeFiles() {
    local nonce=$1 PAD=$2
    nixc build --no-link --print-out-paths --expr "
      let
        system = \"$system\";
        writeFiles = name: spec: derivation {
          inherit name system;
          builder = \"builtin:writeFiles\";
          __structuredAttrs = true;
          __contentAddressed = true;
          outputHashMode = \"recursive\";
          outputHashAlgo = \"sha256\";
          __writeFiles = spec;
        };
      in builtins.genList
        (i: writeFiles \"wf-$nonce-\${toString i}\" { text = \"$nonce-\${toString i}-$PAD\"; })
        $N" >/dev/null
}

run_shellWrite() {
    local nonce=$1 PAD=$2
    nixc build --no-link --print-out-paths --impure --expr "
      let
        system = \"$system\";
        # Reference bash via storePath so it is a tracked build input
        # (otherwise the build sandbox/chroot won't bind its closure).
        bash = builtins.storePath \"$BENCH_BASH\";
        shellWrite = name: text: derivation {
          inherit name system text;
          builder = \"\${bash}/bin/bash\";
          args = [ \"-c\" \"printf %s \\\"\$text\\\" > \$out\" ];
        };
      in builtins.genList
        (i: shellWrite \"sh-$nonce-\${toString i}\" \"$nonce-\${toString i}-$PAD\")
        $N" >/dev/null
}

# One timed iteration: fresh nonce => fresh content => real work.
if [[ "${1:-}" == "_run" ]]; then
    method=$2
    nonce=$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')
    "run_$method" "$nonce" "$(pad)"
    exit 0
fi

# ---- orchestrator ------------------------------------------------------

CHROOT=$(mktemp -d)/store
export CHROOT
mkdir -p "$CHROOT"

echo "nix        : $NIX"
echo "system     : $system"
echo "chroot     : $CHROOT"
echo "files/run  : $N"
echo "approx size: $SIZE bytes"
echo

# The shellWrite (writeText-style) derivation forks bash as its builder.
# A chroot store bind-mounts its own /nix/store for builders, so bash's
# closure must be copied in (the in-process writeFiles builder needs none
# of this). Note: not named BASH — that is a reserved variable bash
# overwrites with its own interpreter path in every subshell.
echo "Seeding bash into the chroot store (for the shellWrite/writeText-style build)..."
BENCH_BASH=$("$NIX" --extra-experimental-features "$FEATURES" build --no-link --print-out-paths 'nixpkgs#bashNonInteractive^out')
export BENCH_BASH
"$NIX" --extra-experimental-features "$FEATURES" copy --no-check-sigs --to "$CHROOT" "$BENCH_BASH"
echo "  bash: $BENCH_BASH"
echo

echo "Running benchmark (via hyperfine)..."
exec "$NIX" --extra-experimental-features "$FEATURES" run nixpkgs#hyperfine -- \
    --warmup "${WARMUP:-2}" --runs "${RUNS:-10}" \
    --command-name toFile     "$HERE/bench.sh _run toFile" \
    --command-name addToStore "$HERE/bench.sh _run addToStore" \
    --command-name writeFiles "$HERE/bench.sh _run writeFiles" \
    --command-name shellWrite "$HERE/bench.sh _run shellWrite"
