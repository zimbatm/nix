#!/usr/bin/env bash
# Benchmark: in-process builtin:fetchurl vs forked builtin:fetchurl
#
# Compares four approaches on the same workload (197 npm tarball FODs):
#   CURL:         curl --parallel (baseline: pure network + TLS, no nix)
#   NEW:          current branch HEAD (in-process, shared FileTransfer, HTTP/2)
#   OLD:          base commit 663db5b48 (fork+sandbox per builtin:fetchurl)
#   pkgs.fetchurl: shell+curl forked in each sandbox
#
# Each nix run uses an isolated nix store + daemon so results are clean.
#
# Usage:
#   ./bench.sh [PARALLELISM]
#
# PARALLELISM defaults to 16. Controls --max-fetch-jobs for the new nix
# and --max-jobs for the old nix.

set -euo pipefail

parallelism="${1:-16}"

dir="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH='' cd -- "$dir/../.." && pwd)"
flake="$dir"
url_json="$dir/urls.json"
workdir=$(mktemp -d /tmp/npm-bench-XXXXXX)
daemon_pid=""

base_commit="663db5b48"

cleanup() {
    echo ""
    echo "Cleaning up..."
    stop_daemon
    if [ "${NIX_BENCH_KEEP:-}" = "1" ]; then
        echo "NIX_BENCH_KEEP=1: preserving workdir at $workdir"
    else
        chmod -R u+w "$workdir" 2>/dev/null || true
        rm -rf "$workdir"
    fi
}
trap cleanup EXIT

# -------------------------------------------------------------------
# Helper functions
# -------------------------------------------------------------------

stop_daemon() {
    if [ -n "$daemon_pid" ] && kill -0 "$daemon_pid" 2>/dev/null; then
        kill "$daemon_pid"
        wait "$daemon_pid" 2>/dev/null || true
    fi
    daemon_pid=""
}

start_daemon() {
    local nix_bin="$1"
    local store_root="$2"
    local daemon_socket="$3"
    local log_file="$4"
    shift 4

    echo "  Starting isolated nix daemon (store: $store_root)..."
    mkdir -p "$store_root"

    env "$@" \
        NIX_DAEMON_SOCKET_PATH="$daemon_socket" \
        "$nix_bin" daemon --store "$store_root" 2>"$log_file" &
    daemon_pid=$!

    for _ in $(seq 1 30); do
        if [ -S "$daemon_socket" ]; then
            break
        fi
        sleep 0.1
    done

    if [ ! -S "$daemon_socket" ]; then
        echo "ERROR: daemon socket did not appear at $daemon_socket"
        exit 1
    fi
    echo "  Daemon ready (pid $daemon_pid)"
}

nix_on() {
    local nix_bin="$1"
    local socket="$2"
    shift 2
    "$nix_bin" --store "unix://$socket" "$@"
}

elapsed_ms() {
    echo $(( ($2 - $1) / 1000000 ))
}

# -------------------------------------------------------------------
# Curl baseline (theoretical floor: pure network + TLS, no nix overhead)
# -------------------------------------------------------------------
num_urls=$(jq length "$url_json")

echo "=== In-process builtin:fetchurl benchmark ==="
echo "Target: jsonld-cli v2.0.0 ($num_urls npm tarball FODs)"
echo "Parallelism: $parallelism"
echo "Working directory: $workdir"
echo ""

# -------------------------------------------------------------------
# Phase 0: curl --parallel baseline (run twice, first warms CDN caches)
# -------------------------------------------------------------------
curl_outdir="$workdir/curl-out"
mkdir -p "$curl_outdir"

# Build curl args: -o <file> <url> for each tarball
curl_args=()
idx=0
while IFS= read -r url; do
    curl_args+=(-o "$curl_outdir/$idx.tgz" "$url")
    idx=$((idx + 1))
done < <(jq -r '.[].url' "$url_json")

echo "--- curl warmup (populating CDN caches) ---"
curl --parallel --parallel-max "$parallelism" -sSf "${curl_args[@]}"
echo "  Done."
echo ""

echo "--- curl --parallel (baseline, single process, connection pooling) ---"
echo "  Fetching $num_urls tarballs with curl --parallel-max $parallelism..."
time_start=$(date +%s%N)
curl --parallel --parallel-max "$parallelism" -sSf "${curl_args[@]}"
time_end=$(date +%s%N)
curl_ms=$(elapsed_ms "$time_start" "$time_end")
echo "  Time: ${curl_ms}ms"
echo ""

# -------------------------------------------------------------------
# Build both nix binaries
# -------------------------------------------------------------------
echo "Building NEW nix (current branch HEAD)..."
new_nix_out=$(nix build "$repo_root" --no-link --print-out-paths --log-format bar-with-logs 2>/dev/null | rg -v man)
new_nix="$new_nix_out/bin/nix"
echo "  $new_nix"

echo "Building OLD nix (base commit $base_commit)..."
old_nix_out=$(nix build "$repo_root?rev=$(git -C "$repo_root" rev-parse "$base_commit")" \
    --no-link --print-out-paths --log-format bar-with-logs 2>/dev/null | rg -v man)
old_nix="$old_nix_out/bin/nix"
echo "  $old_nix"
echo ""

# -------------------------------------------------------------------
# Run benchmark for a given nix binary
# Returns wall-clock time in ms via the global result_ms variable.
# -------------------------------------------------------------------
result_ms=0

run_benchmark() {
    local label="$1"
    local nix_bin="$2"
    local phase_id="$3"
    local target="$4"
    shift 4
    # remaining args are extra nix build flags

    local socket="$workdir/${phase_id}-daemon.sock"
    local store="$workdir/${phase_id}-store"
    local daemon_log="$workdir/${phase_id}-daemon.log"
    local client_log="$workdir/${phase_id}-client.log"

    echo "--- $label ---"

    start_daemon "$nix_bin" "$store" "$socket" "$daemon_log"

    echo "  Pre-populating stdenv closure..."
    nix_on "$nix_bin" "$socket" build "$flake#stdenv-warmup" \
        --no-link \
        --max-jobs 100 \
        --log-format bar-with-logs \
        2>/dev/null || true

    echo "  Building $num_urls FODs ($target)..."
    local time_start time_end
    time_start=$(date +%s%N)
    nix_on "$nix_bin" "$socket" build "$flake#$target" \
        --no-link \
        --log-format bar-with-logs \
        "$@" \
        2>"$client_log"
    time_end=$(date +%s%N)

    result_ms=$(elapsed_ms "$time_start" "$time_end")
    echo "  Time: ${result_ms}ms"
    echo "  Logs: $client_log, $daemon_log"
    echo ""

    stop_daemon
}

# -------------------------------------------------------------------
# Phase 1: OLD nix (fork+sandbox per builtin:fetchurl)
# -------------------------------------------------------------------
run_benchmark \
    "OLD nix (fork+sandbox per fetch, max-jobs=$parallelism)" \
    "$old_nix" \
    "old" \
    "individual" \
    --max-jobs "$parallelism"
old_ms=$result_ms

# -------------------------------------------------------------------
# Phase 2: NEW nix (in-process builtin:fetchurl)
# -------------------------------------------------------------------
run_benchmark \
    "NEW nix (in-process, max-fetch-jobs=$parallelism)" \
    "$new_nix" \
    "new" \
    "individual" \
    --max-jobs 4 \
    --option max-fetch-jobs "$parallelism"
new_ms=$result_ms

# -------------------------------------------------------------------
# Phase 3: NEW nix with substituters enabled (measures cache lookup overhead)
# -------------------------------------------------------------------
run_benchmark \
    "NEW nix + substituters (in-process, max-fetch-jobs=$parallelism)" \
    "$new_nix" \
    "new-subst" \
    "individual-with-subst" \
    --max-jobs 4 \
    --option max-fetch-jobs "$parallelism"
new_subst_ms=$result_ms

# -------------------------------------------------------------------
# Phase 4: pkgs.fetchurl (shell+curl in each sandbox, using new nix)
# -------------------------------------------------------------------
run_benchmark \
    "pkgs.fetchurl (shell+curl per sandbox, max-jobs=$parallelism)" \
    "$new_nix" \
    "pkgs-fetchurl" \
    "pkgs-fetchurl" \
    --max-jobs "$parallelism"
pkgs_fetchurl_ms=$result_ms

# -------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------
echo "=== Summary (parallelism=$parallelism, $num_urls FODs) ==="
printf "%-50s %8sms\n" "curl --parallel (baseline)" "$curl_ms"
printf "%-50s %8sms\n" "NEW builtin:fetchurl (in-process)" "$new_ms"
printf "%-50s %8sms\n" "NEW + substituters (in-process)" "$new_subst_ms"
printf "%-50s %8sms\n" "OLD builtin:fetchurl (fork+sandbox)" "$old_ms"
printf "%-50s %8sms\n" "pkgs.fetchurl (shell+curl per sandbox)" "$pkgs_fetchurl_ms"
echo ""

if [ "$curl_ms" -gt 0 ]; then
    echo "Overhead vs curl baseline:"
    printf "  %-46s %5s%%\n" "NEW builtin:fetchurl (in-process)" \
        "$(awk "BEGIN { printf \"%.0f\", ($new_ms / $curl_ms - 1) * 100 }")"
    printf "  %-46s %5s%%\n" "NEW + substituters (in-process)" \
        "$(awk "BEGIN { printf \"%.0f\", ($new_subst_ms / $curl_ms - 1) * 100 }")"
    printf "  %-46s %5s%%\n" "OLD builtin:fetchurl (fork+sandbox)" \
        "$(awk "BEGIN { printf \"%.0f\", ($old_ms / $curl_ms - 1) * 100 }")"
    printf "  %-46s %5s%%\n" "pkgs.fetchurl (shell+curl per sandbox)" \
        "$(awk "BEGIN { printf \"%.0f\", ($pkgs_fetchurl_ms / $curl_ms - 1) * 100 }")"
    echo ""
fi

if [ "$new_ms" -gt 0 ] && [ "$old_ms" -gt 0 ]; then
    speedup=$(awk "BEGIN { printf \"%.1f\", $old_ms / $new_ms }")
    pct_faster=$(awk "BEGIN { printf \"%.0f\", (1 - $new_ms / $old_ms) * 100 }")
    echo "NEW vs OLD: ${speedup}x faster (${pct_faster}% reduction)"
fi

if [ "$new_ms" -gt 0 ] && [ "$new_subst_ms" -gt 0 ]; then
    subst_overhead=$(awk "BEGIN { printf \"%.0f\", ($new_subst_ms / $new_ms - 1) * 100 }")
    echo "Substituter lookup overhead: +${subst_overhead}%"
fi
