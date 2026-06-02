# Overlay that patches nixpkgs' text-writing trivial builders to use the
# slim `builtin:writeFiles` derivation instead of a full stdenv shell build.
#
# `writeTextFile` is the core that `writeText` / `writeScript` /
# `writeScriptBin` / `writeTextDir` all funnel through. We replace it with a
# file-only derivation that forks no builder, and fall back to the stock
# implementation whenever a `checkPhase` (e.g. a shell dry-run) or custom
# `derivationArgs` is requested — those genuinely need a shell.
#
# Requires the `ca-derivations` experimental feature (the output is
# CA-floating) and a nix that knows `builtin:writeFiles`.
final: prev:
let
  lib = final.lib;

  writeFilesDrv =
    {
      name,
      text,
      executable ? false,
      destination ? "",
      meta ? { },
      passthru ? { },
    }:
    let
      entry = { inherit text; } // lib.optionalAttrs executable { executable = true; };
      spec =
        if destination == "" then entry else { dir.${lib.removePrefix "/" destination} = entry; };
      drv = derivation {
        inherit name;
        system = final.stdenv.hostPlatform.system;
        builder = "builtin:writeFiles";
        __structuredAttrs = true;
        __contentAddressed = true;
        outputHashMode = "recursive";
        outputHashAlgo = "sha256";
        __writeFiles = spec;
      };
    in
    # Re-attach the non-build metadata stock writeTextFile would expose.
    drv // { inherit meta passthru; };

  newWriteTextFile =
    args@{
      name,
      text,
      executable ? false,
      destination ? "",
      checkPhase ? "",
      meta ? { },
      passthru ? { },
      derivationArgs ? { },
      ...
    }:
    if checkPhase == "" && derivationArgs == { } then
      writeFilesDrv { inherit name text executable destination meta passthru; }
    else
      prev.writeTextFile args;
in
{
  writeTextFile = newWriteTextFile;
  writeText = name: text: newWriteTextFile { inherit name text; };
  writeScript = name: text: newWriteTextFile { inherit name text; executable = true; };
  writeScriptBin =
    name: text:
    newWriteTextFile {
      inherit name text;
      executable = true;
      destination = "/bin/${name}";
    };
  writeTextDir =
    path: text:
    newWriteTextFile {
      name = baseNameOf path;
      inherit text;
      destination = "/${path}";
    };
}
