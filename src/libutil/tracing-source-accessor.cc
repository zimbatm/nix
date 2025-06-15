#include "nix/util/tracing-source-accessor.hh"
#include "nix/util/serialise.hh"
#include <fcntl.h>

namespace nix {

TracingSourceAccessor::TracingSourceAccessor(ref<SourceAccessor> inner, const Path & tracePath)
    : inner(inner)
{
    traceFd = toDescriptor(open(tracePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666));
    if (!traceFd)
        throw SysError("opening trace file '%s'", tracePath);
}

void TracingSourceAccessor::traceAccess(
    const CanonPath & path,
    const std::string & operation,
    const std::optional<Stat> & st) const
{
    using namespace std::chrono;

    nlohmann::json entry;
    entry["path"] = path.abs();
    entry["operation"] = operation;
    entry["timestamp"] = duration_cast<microseconds>(
        system_clock::now().time_since_epoch()).count();

    if (st) {
        entry["exists"] = true;

        // Proper type classification
        switch (st->type) {
            case Type::tRegular:
                entry["type"] = "file";
                if (st->fileSize)
                    entry["size"] = *st->fileSize;
                if (st->isExecutable)
                    entry["executable"] = true;
                break;
            case Type::tDirectory:
                entry["type"] = "directory";
                break;
            case Type::tSymlink:
                entry["type"] = "symlink";
                break;
            case Type::tChar:
                entry["type"] = "char_device";
                break;
            case Type::tBlock:
                entry["type"] = "block_device";
                break;
            case Type::tSocket:
                entry["type"] = "socket";
                break;
            case Type::tFifo:
                entry["type"] = "fifo";
                break;
            case Type::tUnknown:
            default:
                entry["type"] = "unknown";
                break;
        }

        // Add modification time if available
        if (st->mtime)
            entry["mtime"] = *st->mtime;

    } else {
        entry["exists"] = false;
    }

    try {
        std::lock_guard<std::mutex> lock(traceMutex);
        writeLine(traceFd.get(), entry.dump());
    } catch (const std::exception & e) {
        // Don't let tracing failures break the actual operation
        // Could add logging here if needed
    }
}

std::string TracingSourceAccessor::readFile(const CanonPath & path)
{
    auto st = inner->maybeLstat(path);
    traceAccess(path, "read", st);
    return inner->readFile(path);
}

bool TracingSourceAccessor::pathExists(const CanonPath & path)
{
    auto exists = inner->pathExists(path);
    auto st = exists ? inner->maybeLstat(path) : std::nullopt;
    traceAccess(path, "stat", st);
    return exists;
}

std::optional<SourceAccessor::Stat> TracingSourceAccessor::maybeLstat(const CanonPath & path)
{
    auto st = inner->maybeLstat(path);
    traceAccess(path, "stat", st);
    return st;
}

SourceAccessor::DirEntries TracingSourceAccessor::readDirectory(const CanonPath & path)
{
    auto st = inner->maybeLstat(path);
    traceAccess(path, "readdir", st);
    return inner->readDirectory(path);
}

std::string TracingSourceAccessor::readLink(const CanonPath & path)
{
    auto st = inner->maybeLstat(path);
    traceAccess(path, "readlink", st);
    return inner->readLink(path);
}

void TracingSourceAccessor::readFile(
    const CanonPath & path,
    Sink & sink,
    std::function<void(uint64_t)> sizeCallback)
{
    auto st = inner->maybeLstat(path);
    traceAccess(path, "read", st);
    return inner->readFile(path, sink, sizeCallback);
}

void TracingSourceAccessor::dumpPath(
    const CanonPath & path,
    Sink & sink,
    PathFilter & filter)
{
    auto st = inner->maybeLstat(path);
    traceAccess(path, "dump", st);
    return inner->dumpPath(path, sink, filter);
}

std::optional<std::filesystem::path> TracingSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    return inner->getPhysicalPath(path);
}

}
