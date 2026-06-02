#!/usr/bin/env bash

source common.sh

# CA-floating outputs require the ca-derivations machinery.
requireDaemonNewerThan "2.34.0pre20251217"
enableFeatures "ca-derivations"

TODO_NixOS

restartDaemon

clearStore

# A leaf node at the top => the output is a flat file, not a directory.
flat=$(nix-build ./writefiles.nix -A flat --no-out-link)
[[ -f "$flat" && ! -d "$flat" ]]
[[ "$(cat "$flat")" == "FLAT" ]]

# Directory tree with the executable bit honoured.
tree=$(nix-build ./writefiles.nix -A tree --no-out-link)
[[ -d "$tree" ]]
[[ "$(cat "$tree/msg")" == "world" ]]
[[ "$(cat "$tree/bin/hello")" == *"echo hi"* ]]
[[ -x "$tree/bin/hello" ]]
[[ ! -x "$tree/msg" ]]

# Build-time references: a writeFiles output may reference (and symlink
# to) another derivation's output, which `builtins.toFile` cannot do.
dep=$(nix-build ./writefiles.nix -A dep --no-out-link)
refs=$(nix-build ./writefiles.nix -A withRefs --no-out-link)
[[ "$(cat "$refs/ref")" == "see $dep" ]]
# The symlink resolves to the dependency's content.
[[ "$(cat "$refs/lnk")" == "DEP CONTENT" ]]
# ... and the dependency is registered as a runtime reference.
nix-store -q --references "$refs" | grep -q "$dep"

# `file` copies a path's bytes verbatim, embedding binary content from
# another derivation's output, and preserves the executable bit.
copied=$(nix-build ./writefiles.nix -A copied --no-out-link)
[[ "$(cat "$copied/data")" == "DEP CONTENT" ]]
[[ "$(cat "$copied/prog")" == *"echo hi"* ]]
[[ -x "$copied/prog" ]]
[[ ! -x "$copied/data" ]]
# Unlike a symlink, copying embeds the bytes and so creates no runtime
# reference to the source (its content does not mention the store path).
[[ -z "$(nix-store -q --references "$copied")" ]]

# Content addressing: rebuilding the same spec yields the same path.
flat2=$(nix-build ./writefiles.nix -A flat --no-out-link)
[[ "$flat" == "$flat2" ]]

# A writeFiles build is realised in-process and holds no build slot, so
# it succeeds even with --max-jobs 0 (which forbids ordinary local
# builds). This exercises the scheduler's inline bypass.
clearStore
slot=$(nix-build ./writefiles.nix -A tree --no-out-link --max-jobs 0)
[[ "$(cat "$slot/msg")" == "world" ]]

echo "writefiles tests passed"
