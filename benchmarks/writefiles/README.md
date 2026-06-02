# `builtin:writeFiles` benchmark

Compares four ways to get file content into the Nix store, to situate the
new `builtin:writeFiles` builder between the eval-time and full-build
extremes:

| method       | mechanism                                            | can reference drv outputs? |
| ------------ | ---------------------------------------------------- | -------------------------- |
| `toFile`     | `builtins.toFile` — written during evaluation        | no                         |
| `addToStore` | `nix-store --add` — copy an existing on-disk file in | n/a                        |
| `writeFiles` | `builtin:writeFiles` — inline build, no fork/sandbox | yes                        |
| `shellWrite` | a `writeText`-style derivation that forks bash       | yes                        |

`shellWrite` stands in for nixpkgs `pkgs.writeText` / `writeTextFile`: a
normal derivation whose builder forks bash to write the file. It is
hand-rolled (rather than pulled from nixpkgs) so the measurement is the
build/fork overhead, not the realisation of stdenv's closure.

## Running

```bash
nix develop --command ./benchmarks/writefiles/bench.sh
# tunables:
N=500 SIZE=1024 RUNS=20 ./benchmarks/writefiles/bench.sh
NIX=/path/to/other/nix ./benchmarks/writefiles/bench.sh
```

It uses the local dev build (the system daemon does not know
`builtin:writeFiles`), preferring an optimized `./build-opt` over the
`-O0` `./build-wf` if present — **build an optimized one, the debug build
is ~2.5x slower and not representative**:

```bash
nix develop --command bash -c 'meson setup build-opt --buildtype=release --optimization=2 && ninja -C build-opt src/nix/nix src/nix/nix-store'
```

It runs everything against a throwaway chroot store under `$TMPDIR` (your
real store is never touched) and reports with hyperfine. Each timed run
uses fresh nonce'd content, so it measures real work rather than cache
hits. Substituters are disabled so only local cost is measured.

## Representative results

100 files of ~64 bytes per invocation, x86_64-linux, warm page cache,
optimized build (`N=100 SIZE=64 RUNS=10`):

```
toFile        109 ms   (1.00x)   eval-time, no derivation
addToStore    122 ms   (1.12x)   store add of an existing file
writeFiles    270 ms   (2.48x)   inline builtin build
shellWrite   1441 ms  (13.21x)   full build, forks a shell
```

Reading it:

- `writeFiles` is ~2.5x slower than `toFile`: it still pays the full
  per-derivation cost (instantiate + register the `.drv`, scratch outputs,
  `registerOutputs`, content-addressing/hashing, CA-realisation + valid-path
  DB registration) that the pure eval-time `toFile` skips. This gap is
  largely fundamental — it *is* a derivation, which is what lets it
  reference (and symlink to) other derivations' outputs, unlike `toFile`.
- But it is ~5x *faster* than the shell-forking build, because it forks no
  builder, sets up no sandbox, and acquires no build user — exactly the gap
  it is meant to fill.

Numbers are dominated by per-invocation `nix` startup at small `N`; raise
`N` to amortise startup and expose per-file cost (~1.6 ms/file for
`writeFiles` on the optimized build).

### What was optimized

Beyond using an optimized build, two changes cut work specific to the
inline builtin path (`build-wf` debug: 690 → 652 ms; the bulk of the
absolute win is the optimized build):

- The build goal no longer computes the sandbox-path FS closure or
  desugars a builder environment for inline builds — a pure file-writing
  builtin needs neither.
- `tryBuildInline()` no longer creates (and tears down) a temporary build
  directory; the builtin writes straight to its scratch outputs.

`perf` on the optimized build shows what is left: ~58% is the `nix`
process's dynamic-linker startup (symbol relocation across its many
`.so`s — a global nix cost, shared with `toFile`, amortised when many
files are built in one invocation), and the remaining per-file build cost
is dominated by sqlite path/realisation registration with no single
hotspot beyond that. The instantiate-`.drv`-then-realise-output work is
inherent to being a derivation.

## Patching nixpkgs (`writefiles-overlay.nix` + `nixpkgs-bench.sh`)

`writefiles-overlay.nix` patches nixpkgs' text-writing trivial builders to
use `builtin:writeFiles`. `writeText` / `writeScript` / `writeScriptBin` /
`writeTextDir` all funnel through `writeTextFile`, which normally does a
full `stdenvNoCC` shell build; the overlay replaces it with a file-only
derivation and falls back to the stock builder whenever a `checkPhase`
(e.g. `writeShellScript`'s shell dry-run) or custom `derivationArgs` is
requested — those genuinely need a shell.

`nixpkgs-bench.sh` builds N freshly-nonce'd `pkgs.writeText` outputs per
invocation through the real nixpkgs machinery, stock vs patched, against a
throwaway chroot store (the stock stdenv closure is seeded once, untimed).

Results (x86_64-linux, optimized build, 256-byte files, medians):

| files/run | stock (shell) | patched (writeFiles) | speedup |
| --------- | ------------- | -------------------- | ------- |
| 50        | 950 ms        | 478 ms               | 2.0x    |
| 400       | 7622 ms       | 909 ms               | 8.4x    |

Both pay the same nixpkgs evaluation cost per invocation, so the
end-to-end speedup grows with N as that shared cost amortises. The
builder-only cost is ~18 ms/file for the stock shell build versus
~1.6 ms/file for `writeFiles` — roughly **11x faster per file**, and the
stock path even had the parallelism advantage (its shell builds run across
cores, while inline `writeFiles` builds run serially).

Caveats: the overlay requires the `ca-derivations` experimental feature
(the output is CA-floating), and its fast path returns a raw `derivation`
with `meta`/`passthru` re-attached — it is a benchmark vehicle, not a
merge-ready patch (a real one would keep full `mkDerivation` semantics
like `.overrideAttrs`).
