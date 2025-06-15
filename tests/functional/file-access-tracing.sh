#!/usr/bin/env bash

source common.sh

clearStore

testDir="$TEST_ROOT/file-access-tracing"
mkdir -p "$testDir"

cat > "$testDir/test.nix" <<EOF
let
  content = builtins.readFile ./data.txt;
  dirEntries = builtins.readDir ./.;
  fileExists = builtins.pathExists ./data.txt;
  missingExists = builtins.pathExists ./missing.txt;
in
{
  inherit content dirEntries fileExists missingExists;
  contentLength = builtins.stringLength content;
}
EOF

echo "Hello from data file" > "$testDir/data.txt"
mkdir -p "$testDir/subdir"
echo "nested content" > "$testDir/subdir/nested.txt"

traceFile="$testDir/trace.jsonl"

(cd "$testDir" && nix eval --trace-file-access "$traceFile" -f test.nix)

[[ -f "$traceFile" ]] || fail "Trace file was not created"

grep -q '"operation":"read"' "$traceFile" || fail "No read operations found in trace"
grep -q '"operation":"stat"' "$traceFile" || fail "No stat operations found in trace"
grep -q '"operation":"readdir"' "$traceFile" || fail "No readdir operations found in trace"

grep -q '"path":".*test.nix"' "$traceFile" || fail "test.nix not found in trace"
grep -q '"path":".*data.txt"' "$traceFile" || fail "data.txt not found in trace"

while IFS= read -r line; do
    if [[ -n "$line" ]]; then
        echo "$line" | jq empty || fail "Invalid JSON in trace: $line"
    fi
done < "$traceFile"

grep -q '"exists":true' "$traceFile" || fail "No existing files traced"
grep -q '"exists":false' "$traceFile" || fail "No non-existing files traced"

grep -q '"type":"file"' "$traceFile" || fail "No files identified as type file"
grep -q '"type":"directory"' "$traceFile" || fail "No directories identified as type directory"

# Check that mtime is included for existing files
jq -e 'select(.exists == true and .type == "file") | has("mtime")' "$traceFile" >/dev/null || fail "No mtime found for existing files"

pureTraceFile="$testDir/pure-trace.jsonl"
(cd "$testDir" && nix eval --pure-eval --trace-file-access "$pureTraceFile" --expr '1 + 1') || true

[[ -f "$pureTraceFile" ]] || fail "Pure eval trace file was not created"

(cd "$testDir" && nix eval --trace-file-access "$testDir/json-trace.jsonl" --json -f test.nix) || fail "Trace flag incompatible with --json"
(cd "$testDir" && nix eval --trace-file-access "$testDir/raw-trace.jsonl" --raw --expr '"hello"') || fail "Trace flag incompatible with --raw"