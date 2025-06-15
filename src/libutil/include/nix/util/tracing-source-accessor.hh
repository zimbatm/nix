#pragma once
///@file

#include "nix/util/source-accessor.hh"
#include "nix/util/file-descriptor.hh"
#include <nlohmann/json.hpp>
#include <chrono>
#include <mutex>

namespace nix {

/**
 * A SourceAccessor wrapper that traces all file system accesses to a JSON Lines file.
 */
class TracingSourceAccessor : public SourceAccessor
{
    ref<SourceAccessor> inner;
    AutoCloseFD traceFd;
    mutable std::mutex traceMutex;

    void traceAccess(
        const CanonPath & path,
        const std::string & operation,
        const std::optional<Stat> & st = std::nullopt) const;

public:
    TracingSourceAccessor(ref<SourceAccessor> inner, const Path & tracePath);

    std::string readFile(const CanonPath & path) override;

    void readFile(
        const CanonPath & path,
        Sink & sink,
        std::function<void(uint64_t)> sizeCallback = [](uint64_t size){}) override;

    bool pathExists(const CanonPath & path) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;

    DirEntries readDirectory(const CanonPath & path) override;

    std::string readLink(const CanonPath & path) override;

    void dumpPath(
        const CanonPath & path,
        Sink & sink,
        PathFilter & filter = defaultPathFilter) override;

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;
};

}