#pragma once
///@file

#include "nix/util/url.hh"
#include "nix/store/binary-cache-store.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/path.hh"
#include "nix/util/hash.hh"
#include "nix/util/sync.hh"

#include <chrono>
#include <tuple>
#include <nlohmann/json_fwd.hpp>

namespace nix {

struct OciBinaryCacheStoreConfig : std::enable_shared_from_this<OciBinaryCacheStoreConfig>,
                                   virtual Store::Config,
                                   BinaryCacheStoreConfig
{
    using BinaryCacheStoreConfig::BinaryCacheStoreConfig;

    OciBinaryCacheStoreConfig(
        std::string_view scheme, std::string_view registryUri, const Store::Config::Params & params);

    /**
     * The parsed OCI registry URL.
     * Format: oci://registry.example.com/repository
     */
    ParsedURL registryUrl;

    /**
     * The registry host (e.g., "ghcr.io", "docker.io").
     */
    std::string registryHost;

    /**
     * The registry port, if specified (0 means use default for scheme).
     */
    uint16_t registryPort = 0;

    /**
     * The repository path (e.g., "myorg/nix-cache").
     */
    std::string repository;

    /**
     * Get the base URL for the registry API, including scheme, host, and port.
     * Uses HTTPS for port 443 or when no port is specified, HTTP otherwise.
     */
    std::string getRegistryBaseUrl() const;

    static const std::string name()
    {
        return "OCI Binary Cache Store";
    }

    static StringSet uriSchemes();

    static std::string doc();

    std::string getHumanReadableURI() const override;

    ref<Store> openStore() const override;

    StoreReference getReference() const override;
};

/**
 * A binary cache store that uses OCI 1.1-compliant registries for storage.
 * Nix closures are stored as multi-layer OCI images, with each store path
 * as a separate layer. This enables parallel downloads and cross-closure
 * deduplication via content-addressable blob digests.
 *
 * URL format: oci://registry.example.com/repository[:tag]
 * Examples:
 *   oci://harbor.example.com/nix-cache:hello-closure
 *   oci://ecr.aws/myorg/nix-cache
 *
 * Closures are addressed by OCI tags. Each layer represents a store path,
 * with NarInfo metadata stored in layer annotations.
 *
 * Requires OCI 1.1: Uses artifactType and layer annotations.
 * Compatible registries: Harbor v2.12+, Amazon ECR, Azure ACR, Zot.
 * Not compatible: Docker Hub, ghcr.io (no OCI 1.1 support yet).
 *
 * Authentication is handled via ~/.docker/config.json, compatible with
 * docker login, podman login, and credential helpers.
 */
class OciBinaryCacheStore : public virtual BinaryCacheStore
{
    struct State
    {
        bool enabled = true;
        std::chrono::steady_clock::time_point disabledUntil;

        /**
         * Cached authentication token for the registry.
         */
        std::string authToken;

        /**
         * When the auth token expires.
         */
        std::chrono::steady_clock::time_point tokenExpiry;
    };

    Sync<State> _state;

    /**
     * Cached credentials from ~/.docker/config.json.
     * First is username, second is password/token.
     */
    std::optional<std::pair<std::string, std::string>> credentials;

public:

    using Config = OciBinaryCacheStoreConfig;

    ref<Config> config;

    OciBinaryCacheStore(ref<Config> config);

    void init() override;

protected:

    void maybeDisable();

    void checkEnabled();

    bool fileExists(const std::string & path) override;

    void upsertFile(
        const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint) override;

    void getFile(const std::string & path, Sink & sink) override;

    void getFile(const std::string & path, Callback<std::optional<std::string>> callback) noexcept override;

    std::optional<std::string> getNixCacheInfo() override;

    std::optional<TrustedFlag> isTrustedClient() override;

private:

    /**
     * Load credentials from ~/.docker/config.json for the registry host.
     */
    void loadDockerCredentials();

    /**
     * Authenticate with the OCI registry and obtain a bearer token.
     * Implements the Docker Registry v2 authentication flow.
     *
     * @param scope The scope for the token (e.g., "repository:myrepo:pull")
     * @return The bearer token
     */
    std::string authenticate(const std::string & scope);

    /**
     * Make an authenticated HTTP request to the OCI registry.
     */
    FileTransferRequest makeRequest(const std::string & path);

    /**
     * Get the manifest for an OCI image by reference (tag or digest).
     *
     * @param reference The tag (hash part) or digest
     * @return The manifest JSON, or std::nullopt if not found
     */
    std::optional<std::string> getManifest(const std::string & reference);

    /**
     * Pull a blob from the OCI registry.
     *
     * @param digest The blob digest (e.g., "sha256:abc123...")
     * @param sink Where to write the blob data
     */
    void pullBlob(const std::string & digest, Sink & sink);

    /**
     * Convert a .narinfo file path to an OCI tag.
     * E.g., "abc123def456.narinfo" -> "abc123def456"
     */
    std::string pathToTag(const std::string & path);

    /**
     * Convert OCI manifest annotations to NarInfo format string.
     * Used for compatibility with the BinaryCacheStore interface.
     */
    std::string manifestToNarInfo(const nlohmann::json & manifest, const std::string & tag);

    /**
     * Extract the tar blob digest from an OCI manifest.
     */
    std::string getTarBlobDigest(const nlohmann::json & manifest);

    /**
     * Information about a single layer in a closure manifest.
     */
    struct LayerInfo
    {
        std::string digest;      // OCI blob digest (sha256:...)
        uint64_t size;           // Blob size in bytes
        StorePath storePath;     // Nix store path
        Hash tarHash;            // Tar content hash for verification
        StorePathSet references; // Dependencies
        std::optional<StorePath> deriver;
        StringSet signatures;
    };

    /**
     * Parse a closure manifest (multi-layer OCI 1.1 format).
     * Returns layer info for each store path in topological order.
     */
    std::vector<LayerInfo> parseClosureManifest(const nlohmann::json & manifest);

    /**
     * Fetch and install a closure from the OCI registry.
     * Downloads missing layers in parallel and unpacks to the destination store.
     *
     * @param tag The OCI tag referencing the closure manifest
     * @param destStore The store to install paths into
     * @param repair Whether to reinstall paths that already exist
     * @return The root store path of the closure
     */
    StorePath fetchClosure(const std::string & tag, Store & destStore, RepairFlag repair = NoRepair);

    /**
     * Query all store paths available in a closure manifest.
     */
    StorePathSet queryClosurePaths(const std::string & tag);

    /**
     * Download and extract a tar layer directly to the destination store.
     *
     * @param layer Layer info including digest and tar hash
     * @param destStore The store to install the path into
     * @param repair Whether to reinstall paths that already exist
     */
    void downloadAndExtractTarLayer(const LayerInfo & layer, Store & destStore, RepairFlag repair);

    // ========== Push Support ==========

    /**
     * Check if a blob already exists in the registry.
     *
     * @param digest The blob digest (e.g., "sha256:abc123...")
     * @return true if blob exists, false otherwise
     */
    bool blobExists(const std::string & digest);

    /**
     * Push a blob to the OCI registry.
     * If the blob already exists (by digest), this is a no-op.
     *
     * @param data The blob data
     * @param digest The expected digest (e.g., "sha256:abc123...")
     */
    void pushBlob(const std::string & data, const std::string & digest);

    /**
     * Push a manifest to the OCI registry.
     *
     * @param manifest The manifest JSON
     * @param reference The tag or digest to push to
     */
    void pushManifest(const nlohmann::json & manifest, const std::string & reference);

    /**
     * Build layer descriptor with annotations for a store path.
     */
    nlohmann::json buildLayerDescriptor(const std::string & digest, uint64_t size, const ValidPathInfo & info, const Hash & tarHash);

    /**
     * Build a closure manifest for the given store paths.
     *
     * @param paths Store paths in topological order (dependencies first)
     * @param pathInfos Map of store path to ValidPathInfo
     * @param layerDigests Map of store path to (digest, size, tarHash) for pushed blobs
     * @param rootPath The root store path of the closure
     * @return The manifest JSON
     */
    nlohmann::json buildClosureManifest(
        const std::vector<StorePath> & paths,
        const std::map<StorePath, ref<const ValidPathInfo>> & pathInfos,
        const std::map<StorePath, std::tuple<std::string, uint64_t, Hash>> & layerDigests,
        const StorePath & rootPath);
};

} // namespace nix
