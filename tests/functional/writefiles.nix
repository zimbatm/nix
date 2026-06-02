let
  system = builtins.currentSystem;

  # A slim, file-only derivation: it runs no builder, the daemon just
  # materialises the node described in `__writeFiles`.
  writeFiles =
    name: spec:
    derivation {
      inherit name system;
      builder = "builtin:writeFiles";
      __structuredAttrs = true;
      __contentAddressed = true;
      outputHashMode = "recursive";
      outputHashAlgo = "sha256";
      __writeFiles = spec;
    };

  dep = writeFiles "wf-dep" { text = "DEP CONTENT\n"; };

  # Directory tree with an executable and a plain file.
  tree = writeFiles "wf-tree" {
    dir = {
      "bin/hello" = {
        text = "#!/bin/sh\necho hi\n";
        executable = true;
      };
      "msg" = { text = "world\n"; };
    };
  };
in
{
  # A leaf node at the top => the output itself is a flat file.
  flat = writeFiles "wf-flat" { text = "FLAT\n"; };

  inherit tree;

  # Build-time references: content and a symlink pointing at another
  # derivation's output (the thing `builtins.toFile` cannot express).
  withRefs = writeFiles "wf-refs" {
    dir = {
      "ref" = { text = "see ${dep}\n"; };
      "lnk" = { symlink = "${dep}"; };
    };
  };

  # `file` copies a path's bytes verbatim, so binary content from another
  # derivation's output can be embedded (not just inline strings). Here we
  # copy the executable `dep/bin/hello`-style script from `tree` and check
  # the executable bit is preserved.
  copied = writeFiles "wf-copied" {
    dir = {
      "data" = { file = "${dep}"; };
      "prog" = { file = "${tree}/bin/hello"; };
    };
  };

  inherit dep;
}
