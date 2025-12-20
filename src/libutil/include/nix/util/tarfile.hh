#pragma once
///@file

#include "nix/util/serialise.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/hash.hh"
#include <archive.h>

namespace nix {

struct TarArchive
{
    struct archive * archive;
    Source * source;
    std::vector<unsigned char> buffer;

    void check(int err, const std::string & reason = "failed to extract archive (%s)");

    explicit TarArchive(const std::filesystem::path & path);

    /// @brief Create a generic archive from source.
    /// @param source - Input byte stream.
    /// @param raw - Whether to enable raw file support. For more info look in docs:
    /// https://manpages.debian.org/stretch/libarchive-dev/archive_read_format.3.en.html
    /// @param compression_method - Primary compression method to use. std::nullopt means 'all'.
    TarArchive(Source & source, bool raw = false, std::optional<std::string> compression_method = std::nullopt);

    /// Disable copy constructor. Explicitly default move assignment/constructor.
    TarArchive(const TarArchive &) = delete;
    TarArchive & operator=(const TarArchive &) = delete;
    TarArchive(TarArchive &&) = default;
    TarArchive & operator=(TarArchive &&) = default;

    void close();

    ~TarArchive();
};

int getArchiveFilterCodeByName(const std::string & method);

void unpackTarfile(Source & source, const std::filesystem::path & destDir);

void unpackTarfile(const std::filesystem::path & tarFile, const std::filesystem::path & destDir);

time_t unpackTarfileToSink(TarArchive & archive, ExtendedFileSystemObjectSink & parseSink);

/**
 * Create a tar archive from a store path.
 *
 * The archive contains entries with paths like "nix/store/xxx-name/..."
 * so that extracting the tar to "/" places files in the correct location.
 *
 * Uses canonical settings for deterministic output:
 * - mtime = 1 (like Nix's mtimeStore)
 * - uid/gid = 0 (root)
 * - mode = 0444/0555 for files, 0555 for directories
 * - Entries sorted alphabetically
 * - POSIX.1-2001 pax restricted format
 *
 * @param storePath The store path to archive (e.g., /nix/store/xxx-name)
 * @param sink Where to write the tar data
 * @return SHA256 hash of the tar content
 */
Hash createStoreTar(const std::filesystem::path & storePath, Sink & sink);

} // namespace nix
