#include <archive.h>
#include <archive_entry.h>

#include "nix/util/finally.hh"
#include "nix/util/serialise.hh"
#include "nix/util/tarfile.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#include <algorithm>
#include <fstream>
#include <set>

namespace nix {

namespace {

int callback_open(struct archive *, void * self)
{
    return ARCHIVE_OK;
}

ssize_t callback_read(struct archive * archive, void * _self, const void ** buffer)
{
    auto self = (TarArchive *) _self;
    *buffer = self->buffer.data();

    try {
        return self->source->read((char *) self->buffer.data(), self->buffer.size());
    } catch (EndOfFile &) {
        return 0;
    } catch (std::exception & err) {
        archive_set_error(archive, EIO, "Source threw exception: %s", err.what());
        return -1;
    }
}

int callback_close(struct archive *, void * self)
{
    return ARCHIVE_OK;
}

void checkLibArchive(archive * archive, int err, const std::string & reason)
{
    if (err == ARCHIVE_EOF)
        throw EndOfFile("reached end of archive");
    else if (err != ARCHIVE_OK)
        throw Error(reason, archive_error_string(archive));
}

constexpr auto defaultBufferSize = std::size_t{65536};
} // namespace

void TarArchive::check(int err, const std::string & reason)
{
    checkLibArchive(archive, err, reason);
}

/// @brief Get filter_code from its name.
///
/// libarchive does not provide a convenience function like archive_write_add_filter_by_name but for reading.
/// Instead it's necessary to use this kludge to convert method -> code and
/// then use archive_read_support_filter_by_code. Arguably this is better than
/// hand-rolling the equivalent function that is better implemented in libarchive.
int getArchiveFilterCodeByName(const std::string & method)
{
    auto * ar = archive_write_new();
    auto cleanup = Finally{[&ar]() { checkLibArchive(ar, archive_write_close(ar), "failed to close archive: %s"); }};
    auto err = archive_write_add_filter_by_name(ar, method.c_str());
    checkLibArchive(ar, err, "failed to get libarchive filter by name: %s");
    auto code = archive_filter_code(ar, 0);
    return code;
}

static void enableSupportedFormats(struct archive * archive)
{
    archive_read_support_format_tar(archive);
    archive_read_support_format_zip(archive);

    /* Enable support for empty files so we don't throw an exception
       for empty HTTP 304 "Not modified" responses. See
       downloadTarball(). */
    archive_read_support_format_empty(archive);
}

TarArchive::TarArchive(Source & source, bool raw, std::optional<std::string> compression_method)
    : archive{archive_read_new()}
    , source{&source}
    , buffer(defaultBufferSize)
{
    if (!compression_method) {
        archive_read_support_filter_all(archive);
    } else {
        archive_read_support_filter_by_code(archive, getArchiveFilterCodeByName(*compression_method));
    }

    if (!raw)
        enableSupportedFormats(archive);
    else {
        archive_read_support_format_raw(archive);
        archive_read_support_format_empty(archive);
    }

    archive_read_set_option(archive, NULL, "mac-ext", NULL);
    check(
        archive_read_open(archive, (void *) this, callback_open, callback_read, callback_close),
        "Failed to open archive (%s)");
}

TarArchive::TarArchive(const std::filesystem::path & path)
    : archive{archive_read_new()}
    , buffer(defaultBufferSize)
{
    archive_read_support_filter_all(archive);
    enableSupportedFormats(archive);
    archive_read_set_option(archive, NULL, "mac-ext", NULL);
    check(archive_read_open_filename(archive, path.string().c_str(), 16384), "failed to open archive: %s");
}

void TarArchive::close()
{
    check(archive_read_close(this->archive), "Failed to close archive (%s)");
}

TarArchive::~TarArchive()
{
    if (this->archive)
        archive_read_free(this->archive);
}

static void extract_archive(TarArchive & archive, const std::filesystem::path & destDir)
{
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT;

    for (;;) {
        struct archive_entry * entry;
        int r = archive_read_next_header(archive.archive, &entry);
        if (r == ARCHIVE_EOF)
            break;
        auto name = archive_entry_pathname(entry);
        if (!name)
            throw Error("cannot get archive member name: %s", archive_error_string(archive.archive));
        if (r == ARCHIVE_WARN)
            warn("getting archive member '%1%': %2%", name, archive_error_string(archive.archive));
        else
            archive.check(r);

        archive_entry_copy_pathname(entry, (destDir / name).string().c_str());

        // sources can and do contain dirs with no rx bits
        if (archive_entry_filetype(entry) == AE_IFDIR && (archive_entry_mode(entry) & 0500) != 0500)
            archive_entry_set_mode(entry, archive_entry_mode(entry) | 0500);

        // Patch hardlink path
        const char * original_hardlink = archive_entry_hardlink(entry);
        if (original_hardlink) {
            archive_entry_copy_hardlink(entry, (destDir / original_hardlink).string().c_str());
        }

        archive.check(archive_read_extract(archive.archive, entry, flags));
    }

    archive.close();
}

void unpackTarfile(Source & source, const std::filesystem::path & destDir)
{
    auto archive = TarArchive(source);

    createDirs(destDir);
    extract_archive(archive, destDir);
}

void unpackTarfile(const std::filesystem::path & tarFile, const std::filesystem::path & destDir)
{
    auto archive = TarArchive(tarFile);

    createDirs(destDir);
    extract_archive(archive, destDir);
}

time_t unpackTarfileToSink(TarArchive & archive, ExtendedFileSystemObjectSink & parseSink)
{
    time_t lastModified = 0;

    /* Only allocate the buffer once. Use the heap because 131 KiB is a bit too
       much for the stack. */
    std::vector<unsigned char> buf(128 * 1024);

    for (;;) {
        // FIXME: merge with extract_archive
        struct archive_entry * entry;
        int r = archive_read_next_header(archive.archive, &entry);
        if (r == ARCHIVE_EOF)
            break;
        auto path = archive_entry_pathname(entry);
        if (!path)
            throw Error("cannot get archive member name: %s", archive_error_string(archive.archive));
        auto cpath = CanonPath{path};
        if (r == ARCHIVE_WARN)
            warn("getting archive member '%1%': %2%", path, archive_error_string(archive.archive));
        else
            archive.check(r);

        lastModified = std::max(lastModified, archive_entry_mtime(entry));

        if (auto target = archive_entry_hardlink(entry)) {
            parseSink.createHardlink(cpath, CanonPath(target));
            continue;
        }

        switch (auto type = archive_entry_filetype(entry)) {

        case AE_IFDIR:
            parseSink.createDirectory(cpath);
            break;

        case AE_IFREG: {
            parseSink.createRegularFile(cpath, [&](auto & crf) {
                if (archive_entry_mode(entry) & S_IXUSR)
                    crf.isExecutable();

                while (true) {
                    auto n = archive_read_data(archive.archive, buf.data(), buf.size());
                    if (n < 0)
                        checkLibArchive(archive.archive, n, "cannot read file from tarball: %s");
                    if (n == 0)
                        break;
                    crf(std::string_view{
                        (const char *) buf.data(),
                        (size_t) n,
                    });
                }
            });

            break;
        }

        case AE_IFLNK: {
            auto target = archive_entry_symlink(entry);

            parseSink.createSymlink(cpath, target);

            break;
        }

        default:
            throw Error("file '%s' in tarball has unsupported file type %d", path, type);
        }
    }

    return lastModified;
}

namespace {

/**
 * Callback for libarchive to write data to a Sink.
 */
struct TarWriteData
{
    Sink * sink;
    HashSink * hashSink;
};

ssize_t tarWriteCallback(struct archive *, void * clientData, const void * buffer, size_t length)
{
    auto * data = static_cast<TarWriteData *>(clientData);
    std::string_view sv(static_cast<const char *>(buffer), length);
    (*data->sink)(sv);
    (*data->hashSink)(sv);
    return length;
}

/**
 * Recursively collect all paths under a directory, sorted for determinism.
 */
void collectPaths(const std::filesystem::path & root, const std::filesystem::path & current, std::set<std::filesystem::path> & paths)
{
    for (const auto & entry : std::filesystem::directory_iterator(current)) {
        auto relPath = std::filesystem::relative(entry.path(), root);
        paths.insert(relPath);
        if (entry.is_directory() && !entry.is_symlink()) {
            collectPaths(root, entry.path(), paths);
        }
    }
}

} // anonymous namespace

Hash createStoreTar(const std::filesystem::path & storePath, Sink & sink)
{
    // Create a write archive with pax restricted format (POSIX.1-2001)
    struct archive * a = archive_write_new();
    if (!a)
        throw Error("failed to create tar archive");

    auto cleanup = Finally([&]() {
        archive_write_free(a);
    });

    archive_write_set_format_pax_restricted(a);

    // Set up callbacks to write to sink and compute hash
    HashSink hashSink(HashAlgorithm::SHA256);
    TarWriteData writeData{&sink, &hashSink};

    archive_write_open(a, &writeData, nullptr, tarWriteCallback, nullptr);

    // Get the parent directory and the store path name
    auto parentDir = storePath.parent_path();
    auto storePathName = storePath.filename();

    // Collect all paths recursively, sorted for determinism
    std::set<std::filesystem::path> relativePaths;
    relativePaths.insert(storePathName); // Include the root directory itself
    collectPaths(parentDir, storePath, relativePaths);

    // Add each entry to the archive
    for (const auto & relPath : relativePaths) {
        auto fullPath = parentDir / relPath;
        auto stat = std::filesystem::symlink_status(fullPath);

        struct archive_entry * entry = archive_entry_new();
        auto entryCleanup = Finally([&]() {
            archive_entry_free(entry);
        });

        // Set the pathname with the store path prefix (e.g., "nix/store/xxx-name/...")
        // We use the path relative to the parent of the store path's parent
        // So for /nix/store/abc-hello/bin/hello, we store as nix/store/abc-hello/bin/hello
        auto archivePath = std::filesystem::path("nix") / "store" / relPath;
        archive_entry_set_pathname(entry, archivePath.string().c_str());

        // Canonical timestamps (mtime = 1, like Nix's mtimeStore)
        archive_entry_set_mtime(entry, 1, 0);
        archive_entry_set_atime(entry, 1, 0);
        archive_entry_set_ctime(entry, 1, 0);

        // Canonical uid/gid
        archive_entry_set_uid(entry, 0);
        archive_entry_set_gid(entry, 0);
        archive_entry_set_uname(entry, "root");
        archive_entry_set_gname(entry, "root");

        if (std::filesystem::is_symlink(stat)) {
            // Symlink
            auto target = std::filesystem::read_symlink(fullPath);
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_symlink(entry, target.string().c_str());
            archive_entry_set_perm(entry, 0777);
        } else if (std::filesystem::is_directory(stat)) {
            // Directory
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0555);
        } else if (std::filesystem::is_regular_file(stat)) {
            // Regular file
            archive_entry_set_filetype(entry, AE_IFREG);
            auto perms = std::filesystem::status(fullPath).permissions();
            bool isExecutable = (perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
            archive_entry_set_perm(entry, isExecutable ? 0555 : 0444);
            archive_entry_set_size(entry, std::filesystem::file_size(fullPath));
        } else {
            throw Error("unsupported file type for '%s'", fullPath.string());
        }

        int r = archive_write_header(a, entry);
        if (r != ARCHIVE_OK)
            throw Error("failed to write tar header for '%s': %s", fullPath.string(), archive_error_string(a));

        // Write file contents for regular files
        if (std::filesystem::is_regular_file(stat) && !std::filesystem::is_symlink(stat)) {
            std::ifstream file(fullPath, std::ios::binary);
            if (!file)
                throw Error("failed to open '%s' for reading", fullPath.string());

            char buffer[65536];
            while (file) {
                file.read(buffer, sizeof(buffer));
                auto bytesRead = file.gcount();
                if (bytesRead > 0) {
                    if (archive_write_data(a, buffer, bytesRead) < 0)
                        throw Error("failed to write data for '%s': %s", fullPath.string(), archive_error_string(a));
                }
            }
        }
    }

    archive_write_close(a);

    return hashSink.finish().hash;
}

} // namespace nix
