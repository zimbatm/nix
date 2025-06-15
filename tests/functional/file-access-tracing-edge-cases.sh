#!/usr/bin/env bash

source common.sh

clearStore

testDir="$TEST_ROOT/file-access-tracing-edge-cases"
mkdir -p "$testDir"

# Test invalid trace file path
invalidTraceFile="/nonexistent/directory/trace.jsonl"
if (cd "$testDir" && nix eval --impure --trace-file-access "$invalidTraceFile" --expr '1 + 1' 2>/dev/null); then
    fail "Should have failed with invalid trace file path"
fi

# Test permission denied on trace file
permissionDir="$testDir/no-write"
mkdir -p "$permissionDir"
chmod 555 "$permissionDir"
permissionTraceFile="$permissionDir/trace.jsonl"

if (cd "$testDir" && nix eval --impure --trace-file-access "$permissionTraceFile" --expr '1 + 1' 2>/dev/null); then
    fail "Should have failed with permission denied"
fi
chmod 755 "$permissionDir"

# Test trace file in existing directory
existingTraceFile="$testDir/existing-trace.jsonl"
(cd "$testDir" && nix eval --impure --trace-file-access "$existingTraceFile" --expr '"hello"' >/dev/null)
[[ -f "$existingTraceFile" ]] || fail "Trace file should have been created"

# Test very long file paths
longDir="$testDir/$(printf 'a%.0s' {1..100})"
mkdir -p "$longDir"
echo "long path content" > "$longDir/file.txt"
longTraceFile="$testDir/long-path-trace.jsonl"

(cd "$testDir" && nix eval --impure --trace-file-access "$longTraceFile" --expr 'builtins.readFile ./'"$(basename "$longDir")"'/file.txt' >/dev/null)
grep -q "$longDir" "$longTraceFile" || fail "Long path should appear in trace"

# Test special characters in filenames
specialDir="$testDir/special-chars"
mkdir -p "$specialDir"
echo "content1" > "$specialDir/file with spaces.txt"
echo "content2" > "$specialDir/file-with-dashes.txt"
echo "content3" > "$specialDir/file_with_underscores.txt"
echo "content4" > "$specialDir/file.with.dots.txt"

specialTraceFile="$testDir/special-chars-trace.jsonl"
(cd "$testDir" && nix eval --impure --trace-file-access "$specialTraceFile" --expr 'builtins.readDir ./special-chars' >/dev/null)

while IFS= read -r line; do
    if [[ -n "$line" ]]; then
        echo "$line" | jq empty || fail "Invalid JSON with special characters: $line"
    fi
done < "$specialTraceFile"

# Test symlinks
symlinkDir="$testDir/symlinks"
mkdir -p "$symlinkDir"
echo "target content" > "$symlinkDir/target.txt"
ln -sf target.txt "$symlinkDir/link.txt"
ln -sf nonexistent.txt "$symlinkDir/broken-link.txt"

symlinkTraceFile="$testDir/symlink-trace.jsonl"
(cd "$testDir" && nix eval --impure --trace-file-access "$symlinkTraceFile" --expr 'builtins.readFile ./symlinks/link.txt' >/dev/null)

grep -q '"type":"symlink"' "$symlinkTraceFile" || fail "Symlinks should be identified as type 'symlink'"

# Test non-existent files
nonexistentTraceFile="$testDir/nonexistent-trace.jsonl"
(cd "$testDir" && nix eval --impure --trace-file-access "$nonexistentTraceFile" --expr 'builtins.pathExists ./does-not-exist.txt' >/dev/null)

grep -q '"exists":false' "$nonexistentTraceFile" || fail "Non-existent files should be traced with exists=false"

# Test large number of file operations
manyFilesDir="$testDir/many-files"
mkdir -p "$manyFilesDir"
for i in {1..50}; do
    echo "file $i content" > "$manyFilesDir/file$i.txt"
done

manyFilesTraceFile="$testDir/many-files-trace.jsonl"
(cd "$testDir" && nix eval --impure --trace-file-access "$manyFilesTraceFile" --expr 'builtins.readDir ./many-files' >/dev/null)

traceCount=$(wc -l < "$manyFilesTraceFile")
if [[ $traceCount -lt 2 ]]; then
    fail "Expected at least 2 trace entries, got $traceCount"
fi

# Test concurrent evaluation
concurrentTraceFile="$testDir/concurrent-trace.jsonl"

(cd "$testDir" && nix eval --impure --trace-file-access "$concurrentTraceFile" --expr 'builtins.readFile ./README.md' >/dev/null || true) &
(cd "$testDir" && nix eval --impure --trace-file-access "$concurrentTraceFile" --expr 'builtins.readDir ./.' >/dev/null) &
wait

while IFS= read -r line; do
    if [[ -n "$line" ]]; then
        echo "$line" | jq empty || fail "Concurrent writes produced invalid JSON: $line"
    fi
done < "$concurrentTraceFile"

# Test trace file format validation
formatTraceFile="$testDir/format-trace.jsonl"
(cd "$testDir" && touch README.md && nix eval --impure --trace-file-access "$formatTraceFile" --expr '{
  file = builtins.readFile ./README.md;
  dir = builtins.readDir ./.;
  exists = builtins.pathExists ./README.md;
  missing = builtins.pathExists ./nonexistent;
}' >/dev/null)

while IFS= read -r line; do
    if [[ -n "$line" ]]; then
        entry=$(echo "$line" | jq .)

        for field in path operation timestamp exists; do
            if ! echo "$entry" | jq -e "has(\"$field\")" >/dev/null; then
                fail "Missing required field '$field' in trace entry: $line"
            fi
        done

        if echo "$entry" | jq -e '.exists and (.exists == true)' >/dev/null; then
            if ! echo "$entry" | jq -e 'has("type")' >/dev/null; then
                fail "Missing type field for existing file: $line"
            fi
        fi
    fi
done < "$formatTraceFile"
