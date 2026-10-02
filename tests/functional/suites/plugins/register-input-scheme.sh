# shellcheck shell=bash

# A plugin can register an input scheme. The scheme must be able to
# claim a local repository, so that path-like flake references below
# that repository resolve to the plugin's scheme.

plugin=$(findPlugin libplugintest_inputscheme)

repo=$TEST_ROOT/dummy-repo
rm -rf "$repo"
mkdir -p "$repo/.dummy" "$repo/sub"

cat > "$repo/sub/flake.nix" <<EOF
{
  outputs = { self }: { };
}
EOF

# Without the plugin, nothing knows about '.dummy'.
[[ $(nix flake metadata --json "$repo/sub" | jq -r .resolvedUrl) = "path:$repo/sub" ]]

# With the plugin, the flake reference resolves to the repository root.
resolved=$(nix --option plugin-files "$plugin" flake metadata --json "$repo/sub" | jq -r .resolvedUrl)
[[ $resolved = "dummy+file://$repo?dir=sub" ]]

# Other code paths that search for a local repository also see the
# plugin's scheme.
expectStderr 1 nix --option plugin-files "$plugin" eval --impure --expr "builtins.fetchTarball \"file://$repo\"" |
    grepQuiet "Please use .dummy+file. as the scheme"
