#include "nix/store/builtins.hh"
#include "nix/store/derivations.hh"
#include "nix/util/file-system.hh"

#include <nlohmann/json.hpp>
#include <filesystem>

namespace nix {

/**
 * Validate a relative path used as a key in a `dir` node. It must be
 * relative and non-empty, and must not contain "." or ".." components,
 * so that it cannot escape the output directory.
 */
static std::filesystem::path checkWriteFilesPath(std::string_view raw)
{
    std::filesystem::path p{raw};
    if (p.empty() || p.is_absolute())
        throw Error("builtin:writeFiles: path '%s' must be relative and non-empty", raw);
    for (auto & component : p) {
        auto c = component.string();
        if (c == "." || c == "..")
            throw Error("builtin:writeFiles: path '%s' must not contain '.' or '..' components", raw);
    }
    return p;
}

/**
 * Materialise one node of the `__writeFiles` spec at `dst`.
 *
 * A node is one of:
 *
 *   { "text": "...", "executable": true|false }   // a regular file
 *   { "file": "/abs/path", "executable": ... }    // copy content from a path
 *   { "symlink": "..." }                           // a symlink
 *   { "dir": { "<rel-path>": <node>, ... } }       // a directory tree
 *
 * `text` writes inline content; `file` copies a path's bytes verbatim
 * (so binary content, e.g. another derivation's output, can be embedded)
 * and preserves the source's executable bit unless `executable` overrides
 * it. `dir` keys are output-relative paths (which may contain `/`) mapped
 * to further nodes, so trees nest arbitrarily.
 */
static void materialiseNode(const std::filesystem::path & dst, const nlohmann::json & node)
{
    if (!node.is_object())
        throw Error("builtin:writeFiles: each node must be an object");

    auto isLeaf = node.contains("text") || node.contains("file") || node.contains("symlink");

    if (auto dir = node.find("dir"); dir != node.end()) {
        if (isLeaf)
            throw Error("builtin:writeFiles: a 'dir' node must not also have 'text', 'file', or 'symlink'");
        if (!dir->is_object())
            throw Error("builtin:writeFiles: 'dir' must be an object mapping paths to nodes");
        createDirs(dst);
        for (auto & [rawPath, child] : dir->items())
            materialiseNode(dst / checkWriteFilesPath(rawPath), child);
        return;
    }

    createDirs(dst.parent_path());

    if (auto symlink = node.find("symlink"); symlink != node.end()) {
        createSymlink(symlink->get<std::string>(), dst);
    } else if (auto text = node.find("text"); text != node.end()) {
        bool executable = node.value("executable", false);
        writeFile(dst, text->get<std::string>(), executable ? 0755 : 0644);
    } else if (auto file = node.find("file"); file != node.end()) {
        std::filesystem::path src{file->get<std::string>()};
        if (!src.is_absolute())
            throw Error("builtin:writeFiles: 'file' source '%s' must be an absolute path", src.string());
        /* Copy the bytes (dereferencing a source symlink), preserving the
           source's mode — and hence its executable bit — by default. */
        copyFile(src, dst, /*andDelete=*/false, /*contents=*/true);
        if (auto executable = node.find("executable"); executable != node.end())
            chmod(dst, executable->get<bool>() ? 0755 : 0644);
    } else {
        throw Error("builtin:writeFiles: node must have a 'text', 'file', 'symlink', or 'dir' attribute");
    }
}

/**
 * A slim, file-only builder: it executes no program, it only
 * materialises a fixed set of regular files and symlinks described in
 * the derivation's structured attributes. Because nothing is executed,
 * it is pure by construction.
 *
 * The spec lives under the reserved `__writeFiles` structured attribute
 * and is a single node (see `materialiseNode`). A leaf node (`text` /
 * `file` / `symlink`) makes the output itself a flat file or symlink
 * (matching `builtins.toFile` / `writeText`); a `dir` node makes it a
 * directory tree.
 */
static void builtinWriteFiles(const BuiltinBuilderContext & ctx)
{
    if (!ctx.drv.structuredAttrs)
        throw Error("builtin:writeFiles requires __structuredAttrs to be set");

    nlohmann::json attrs = ctx.drv.structuredAttrs->structuredAttrs;

    auto i = attrs.find("__writeFiles");
    if (i == attrs.end())
        throw Error("builtin:writeFiles: missing '__writeFiles' structured attribute");

    materialiseNode(std::filesystem::path{ctx.outputs.at("out")}, *i);
}

static RegisterBuiltinBuilder registerWriteFiles("writeFiles", builtinWriteFiles);

} // namespace nix
