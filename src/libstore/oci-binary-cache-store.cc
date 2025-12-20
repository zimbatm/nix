#include "nix/store/oci-binary-cache-store.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/globals.hh"
#include "nix/store/nar-info.hh"
#include "nix/store/nar-info-disk-cache.hh"
#include "nix/store/store-registration.hh"
#include "nix/util/archive.hh"
#include "nix/util/base-n.hh"
#include "nix/util/callback.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/tarfile.hh"
#include "nix/util/thread-pool.hh"

#include <nlohmann/json.hpp>
#include <regex>
#include <fstream>

namespace nix {

// OCI Distribution Specification media types
static const std::string ociManifestMediaType = "application/vnd.oci.image.manifest.v1+json";
static const std::string ociConfigMediaType = "application/vnd.nix.closure-info.v1+json";
// Standard OCI layer type - tar format for direct extraction
static const std::string ociLayerMediaType = "application/vnd.oci.image.layer.v1.tar";
// OCI 1.1 artifact type for Nix closures
static const std::string ociClosureArtifactType = "application/vnd.nix.closure.v1";

// Annotation keys for layer metadata
static const std::string annotationStorePath = "org.nixos.store-path";
static const std::string annotationTarHash = "org.nixos.tar-hash";
static const std::string annotationReferences = "org.nixos.references";
static const std::string annotationDeriver = "org.nixos.deriver";
static const std::string annotationSignatures = "org.nixos.signatures";
static const std::string annotationCA = "org.nixos.ca";

// Manifest-level annotations for closures
static const std::string annotationRootPath = "org.nixos.root-path";
static const std::string annotationClosureSize = "org.nixos.closure-size";

StringSet OciBinaryCacheStoreConfig::uriSchemes()
{
    return {"oci"};
}

OciBinaryCacheStoreConfig::OciBinaryCacheStoreConfig(
    std::string_view scheme, std::string_view _registryUri, const Params & params)
    : StoreConfig(params)
    , BinaryCacheStoreConfig(params)
{
    if (_registryUri.empty())
        throw UsageError("OCI store requires a registry URL in the form 'oci://registry.example.com/repository'");

    // Parse the registry URI
    // Input format: "registry.example.com/repository" (without scheme://)
    auto fullUri = std::string{scheme} + "://" + std::string{_registryUri};
    registryUrl = parseURL(fullUri);

    // The host is the registry
    if (!registryUrl.authority)
        throw UsageError("OCI store URL must include a registry host");
    registryHost = registryUrl.authority->host;
    if (registryHost.empty())
        throw UsageError("OCI store URL must include a registry host");

    // Extract port if specified
    registryPort = registryUrl.authority->port.value_or(0);

    // The path is the repository
    // Remove leading slashes and join path components
    std::string repoPath;
    for (const auto & segment : registryUrl.path) {
        if (!segment.empty()) {
            if (!repoPath.empty())
                repoPath += "/";
            repoPath += segment;
        }
    }

    if (repoPath.empty())
        throw UsageError("OCI store URL must include a repository path (e.g., oci://ghcr.io/myorg/nix-cache)");

    repository = repoPath;
}

std::string OciBinaryCacheStoreConfig::getHumanReadableURI() const
{
    if (registryPort != 0 && registryPort != 443)
        return "oci://" + registryHost + ":" + std::to_string(registryPort) + "/" + repository;
    return "oci://" + registryHost + "/" + repository;
}

std::string OciBinaryCacheStoreConfig::getRegistryBaseUrl() const
{
    // Use HTTP for non-standard ports (like 5000), HTTPS for port 443 or default
    bool useHttps = (registryPort == 0 || registryPort == 443);
    std::string scheme = useHttps ? "https" : "http";
    std::string portSuffix;
    if (registryPort != 0) {
        // Always include port when specified, even for 443
        portSuffix = ":" + std::to_string(registryPort);
    }
    return scheme + "://" + registryHost + portSuffix;
}

StoreReference OciBinaryCacheStoreConfig::getReference() const
{
    return {
        .variant =
            StoreReference::Specified{
                .scheme = "oci",
                .authority = registryHost + "/" + repository,
            },
        .params = getQueryParams(),
    };
}

std::string OciBinaryCacheStoreConfig::doc()
{
    return
#include "oci-binary-cache-store.md"
        ;
}

OciBinaryCacheStore::OciBinaryCacheStore(ref<Config> config)
    : Store{*config}
    , BinaryCacheStore{*config}
    , config{config}
{
    diskCache = getNarInfoDiskCache();
    loadDockerCredentials();
}

void OciBinaryCacheStore::init()
{
    // For OCI stores, we don't need to check for nix-cache-info
    // since OCI registries don't use that convention.
    // Just verify we can access the registry.
    auto cacheKey = config->getReference().render(/*withParams=*/false);

    if (auto cacheInfo = diskCache->upToDateCacheExists(cacheKey)) {
        config->wantMassQuery.setDefault(cacheInfo->wantMassQuery);
        config->priority.setDefault(cacheInfo->priority);
    } else {
        // Try to authenticate with the registry to verify access
        try {
            authenticate("repository:" + config->repository + ":pull");
        } catch (Error & e) {
            throw Error("cannot access OCI registry '%s': %s", config->getHumanReadableURI(), e.what());
        }

        // Create cache entry with defaults
        diskCache->createCache(cacheKey, config->storeDir, config->wantMassQuery, config->priority);
    }
}

void OciBinaryCacheStore::loadDockerCredentials()
{
    // Try to load credentials from ~/.docker/config.json
    auto configPath = getHome() + "/.docker/config.json";

    if (!pathExists(configPath)) {
        debug("no Docker config found at %s", configPath);
        return;
    }

    try {
        auto configContent = readFile(configPath);
        auto json = nlohmann::json::parse(configContent);

        if (!json.contains("auths"))
            return;

        const auto & auths = json["auths"];

        // Try the exact registry host first
        std::string host = config->registryHost;

        // For Docker Hub, the config uses "https://index.docker.io/v1/"
        if (host == "docker.io" || host == "registry-1.docker.io") {
            host = "https://index.docker.io/v1/";
        }

        if (auths.contains(host)) {
            const auto & authEntry = auths[host];
            if (authEntry.contains("auth")) {
                // Base64 encoded "username:password"
                auto decoded = base64::decode(authEntry["auth"].get<std::string>());
                auto colonPos = decoded.find(':');
                if (colonPos != std::string::npos) {
                    credentials = std::make_pair(decoded.substr(0, colonPos), decoded.substr(colonPos + 1));
                    debug("loaded Docker credentials for %s", host);
                }
            }
        }
    } catch (std::exception & e) {
        warn("failed to parse Docker config at %s: %s", configPath, e.what());
    }
}

std::string OciBinaryCacheStore::authenticate(const std::string & scope)
{
    auto state(_state.lock());

    // Check if we have a valid cached token
    if (!state->authToken.empty() && std::chrono::steady_clock::now() < state->tokenExpiry) {
        return state->authToken;
    }

    // Construct the registry API base URL
    std::string registryBase = config->getRegistryBaseUrl();

    // First, try to access without authentication (anonymous)
    FileTransferRequest pingRequest(registryBase + "/v2/");
    pingRequest.method = HttpMethod::Get;

    try {
        auto result = getFileTransfer()->download(pingRequest);
        // If we got here, no auth needed
        state->authToken = "";
        state->tokenExpiry = std::chrono::steady_clock::now() + std::chrono::hours(1);
        return "";
    } catch (FileTransferError & e) {
        // 401/403 errors are mapped to Forbidden in filetransfer.cc
        if (e.error != FileTransfer::Forbidden) {
            throw;
        }
        // Need to authenticate - parse WWW-Authenticate header
    }

    // Parse the WWW-Authenticate header to find the auth service
    // Format: Bearer realm="https://auth.docker.io/token",service="registry.docker.io"
    // For now, use common defaults

    std::string authRealm;
    std::string authService = config->registryHost;

    // Common auth endpoints
    if (config->registryHost == "ghcr.io") {
        authRealm = "https://ghcr.io/token";
        authService = "ghcr.io";
    } else if (config->registryHost == "docker.io" || config->registryHost == "registry-1.docker.io") {
        authRealm = "https://auth.docker.io/token";
        authService = "registry.docker.io";
    } else if (config->registryHost == "quay.io") {
        authRealm = "https://quay.io/v2/auth";
        authService = "quay.io";
    } else {
        // Generic fallback - try the registry itself
        authRealm = registryBase + "/v2/token";
    }

    // Request token from auth service
    auto tokenUrl = authRealm + "?service=" + percentEncode(authService) + "&scope=" + percentEncode(scope);

    FileTransferRequest tokenRequest(tokenUrl);
    tokenRequest.method = HttpMethod::Get;

    if (credentials) {
        // Add Basic auth for the token request
        auto authString = credentials->first + ":" + credentials->second;
        std::span<const std::byte> authBytes(reinterpret_cast<const std::byte *>(authString.data()), authString.size());
        tokenRequest.headers.push_back(
            std::make_pair(std::string("Authorization"), "Basic " + base64::encode(authBytes)));
    }

    try {
        auto result = getFileTransfer()->download(tokenRequest);
        auto json = nlohmann::json::parse(result.data);

        std::string token;
        if (json.contains("token")) {
            token = json["token"].get<std::string>();
        } else if (json.contains("access_token")) {
            token = json["access_token"].get<std::string>();
        } else {
            throw Error("OCI registry auth response missing token");
        }

        // Parse expiry if provided (default to 5 minutes)
        int expiresIn = 300;
        if (json.contains("expires_in")) {
            expiresIn = json["expires_in"].get<int>();
        }

        state->authToken = token;
        state->tokenExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(expiresIn - 30);

        return token;
    } catch (FileTransferError & e) {
        throw Error("failed to authenticate with OCI registry '%s': %s", config->registryHost, e.message());
    }
}

FileTransferRequest OciBinaryCacheStore::makeRequest(const std::string & path)
{
    std::string url = config->getRegistryBaseUrl() + path;
    FileTransferRequest request(url);

    // Add authentication header if we have a token
    auto state(_state.lock());
    if (!state->authToken.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + state->authToken});
    }

    return request;
}

void OciBinaryCacheStore::maybeDisable()
{
    auto state(_state.lock());
    if (state->enabled && settings.tryFallback) {
        int t = 60;
        printError("disabling OCI binary cache '%s' for %s seconds", config->getHumanReadableURI(), t);
        state->enabled = false;
        state->disabledUntil = std::chrono::steady_clock::now() + std::chrono::seconds(t);
    }
}

void OciBinaryCacheStore::checkEnabled()
{
    auto state(_state.lock());
    if (state->enabled)
        return;
    if (std::chrono::steady_clock::now() > state->disabledUntil) {
        state->enabled = true;
        debug("re-enabling OCI binary cache '%s'", config->getHumanReadableURI());
        return;
    }
    throw SubstituterDisabled("OCI substituter '%s' is disabled", config->getHumanReadableURI());
}

std::string OciBinaryCacheStore::pathToTag(const std::string & path)
{
    // Convert "abc123def456.narinfo" to "abc123def456"
    // or "nar/abc123.nar.zst" to the hash part
    if (hasSuffix(path, ".narinfo")) {
        return path.substr(0, path.size() - 8); // Remove ".narinfo"
    }
    // For NAR files, extract the hash from the URL stored in the manifest
    throw Error("cannot convert path '%s' to OCI tag", path);
}

std::optional<std::string> OciBinaryCacheStore::getManifest(const std::string & reference)
{
    // Ensure we have a valid auth token
    authenticate("repository:" + config->repository + ":pull");

    auto path = "/v2/" + config->repository + "/manifests/" + reference;
    auto request = makeRequest(path);
    request.method = HttpMethod::Get;
    request.headers.push_back({"Accept", ociManifestMediaType});

    try {
        auto result = getFileTransfer()->download(request);
        return result.data;
    } catch (FileTransferError & e) {
        if (e.error == FileTransfer::NotFound)
            return std::nullopt;
        throw;
    }
}

void OciBinaryCacheStore::pullBlob(const std::string & digest, Sink & sink)
{
    // Ensure we have a valid auth token
    authenticate("repository:" + config->repository + ":pull");

    auto path = "/v2/" + config->repository + "/blobs/" + digest;
    auto request = makeRequest(path);
    request.method = HttpMethod::Get;

    try {
        getFileTransfer()->download(std::move(request), sink);
    } catch (FileTransferError & e) {
        if (e.error == FileTransfer::NotFound)
            throw NoSuchBinaryCacheFile("blob '%s' not found in OCI registry", digest);
        throw;
    }
}

std::string OciBinaryCacheStore::manifestToNarInfo(const nlohmann::json & manifest, const std::string & tag)
{
    // Reconstruct NarInfo from OCI manifest annotations
    // Note: For tar-based layers, we use the tar hash for file verification,
    // but still need to provide NarInfo format for compatibility
    std::string narInfo;

    const auto & annotations = manifest.value("annotations", nlohmann::json::object());

    // StorePath is required
    if (!annotations.contains(annotationStorePath))
        throw Error("OCI manifest missing required annotation '%s'", annotationStorePath);

    narInfo += "StorePath: " + annotations[annotationStorePath].get<std::string>() + "\n";

    // URL - point to the blob
    if (manifest.contains("layers") && !manifest["layers"].empty()) {
        auto digest = manifest["layers"][0]["digest"].get<std::string>();
        debug("OCI pull: manifestToNarInfo layer digest=%s, config digest=%s",
              digest,
              manifest.contains("config") ? manifest["config"]["digest"].get<std::string>() : "(no config)");
        // The URL is relative to the binary cache, but for OCI we use a special format
        narInfo += "URL: oci-blob://" + digest + "\n";
    }

    // For tar-based layers, compression is not used (tar is uncompressed in registry)
    narInfo += "Compression: none\n";

    // For tar-based layers, we use the tar hash as the file hash
    // NAR hash will be computed on extraction
    if (annotations.contains(annotationTarHash)) {
        // Use tar hash as a placeholder for NarHash - actual verification is done via tar hash
        narInfo += "NarHash: " + annotations[annotationTarHash].get<std::string>() + "\n";
    }

    // FileHash and FileSize from the layer
    if (manifest.contains("layers") && !manifest["layers"].empty()) {
        const auto & layer = manifest["layers"][0];
        if (layer.contains("digest")) {
            auto digest = layer["digest"].get<std::string>();
            // OCI digests are in format "sha256:abc123..."
            if (hasPrefix(digest, "sha256:")) {
                narInfo += "FileHash: sha256:" + digest.substr(7) + "\n";
            }
        }
        if (layer.contains("size")) {
            narInfo += "FileSize: " + std::to_string(layer["size"].get<uint64_t>()) + "\n";
            narInfo += "NarSize: " + std::to_string(layer["size"].get<uint64_t>()) + "\n";
        }
    }

    // References
    if (annotations.contains(annotationReferences)) {
        auto refs = annotations[annotationReferences].get<std::string>();
        if (!refs.empty())
            narInfo += "References: " + refs + "\n";
    }

    // Deriver
    if (annotations.contains(annotationDeriver)) {
        auto deriver = annotations[annotationDeriver].get<std::string>();
        if (!deriver.empty())
            narInfo += "Deriver: " + deriver + "\n";
    }

    // Signatures
    if (annotations.contains(annotationSignatures)) {
        auto sigs = annotations[annotationSignatures].get<std::string>();
        for (const auto & sig : tokenizeString<std::vector<std::string>>(sigs, " ")) {
            narInfo += "Sig: " + sig + "\n";
        }
    }

    // CA (content address)
    if (annotations.contains(annotationCA)) {
        narInfo += "CA: " + annotations[annotationCA].get<std::string>() + "\n";
    }

    return narInfo;
}

std::string OciBinaryCacheStore::getTarBlobDigest(const nlohmann::json & manifest)
{
    if (!manifest.contains("layers") || manifest["layers"].empty())
        throw Error("OCI manifest has no layers");

    auto digest = manifest["layers"][0]["digest"].get<std::string>();
    debug("OCI pull: getTarBlobDigest returning %s from manifest with %d layers", digest, manifest["layers"].size());
    return digest;
}

bool OciBinaryCacheStore::fileExists(const std::string & path)
{
    checkEnabled();

    try {
        if (hasSuffix(path, ".narinfo")) {
            auto tag = pathToTag(path);
            auto manifest = getManifest(tag);
            return manifest.has_value();
        }
        // For other files (NAR blobs), check if the blob exists
        // This is typically called with a blob digest
        if (hasPrefix(path, "oci-blob://")) {
            auto digest = path.substr(11); // Remove "oci-blob://"
            authenticate("repository:" + config->repository + ":pull");
            auto requestPath = "/v2/" + config->repository + "/blobs/" + digest;
            auto request = makeRequest(requestPath);
            request.method = HttpMethod::Head;
            getFileTransfer()->download(request);
            return true;
        }
        return false;
    } catch (FileTransferError & e) {
        if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
            return false;
        maybeDisable();
        throw;
    }
}

void OciBinaryCacheStore::getFile(const std::string & path, Sink & sink)
{
    checkEnabled();

    try {
        if (hasSuffix(path, ".narinfo")) {
            // Fetch the OCI manifest and convert to NarInfo format
            auto tag = pathToTag(path);
            auto manifestJson = getManifest(tag);
            if (!manifestJson)
                throw NoSuchBinaryCacheFile("store path '%s' not found in OCI registry", tag);

            auto manifest = nlohmann::json::parse(*manifestJson);
            auto narInfo = manifestToNarInfo(manifest, tag);
            sink(narInfo);
        } else if (hasPrefix(path, "oci-blob://")) {
            // Fetch a blob directly
            auto digest = path.substr(11); // Remove "oci-blob://"
            pullBlob(digest, sink);
        } else {
            throw NoSuchBinaryCacheFile("unknown file type in OCI store: '%s'", path);
        }
    } catch (FileTransferError & e) {
        if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
            throw NoSuchBinaryCacheFile(
                "file '%s' not found in OCI registry '%s'", path, config->getHumanReadableURI());
        maybeDisable();
        throw;
    }
}

void OciBinaryCacheStore::getFile(const std::string & path, Callback<std::optional<std::string>> callback) noexcept
{
    try {
        StringSink sink;
        getFile(path, sink);
        callback(std::move(sink.s));
    } catch (NoSuchBinaryCacheFile &) {
        callback(std::nullopt);
    } catch (...) {
        callback.rethrow();
    }
}

std::vector<OciBinaryCacheStore::LayerInfo> OciBinaryCacheStore::parseClosureManifest(const nlohmann::json & manifest)
{
    std::vector<LayerInfo> layers;

    // Verify this is an OCI 1.1 closure manifest
    if (manifest.contains("artifactType")) {
        auto artifactType = manifest["artifactType"].get<std::string>();
        if (artifactType != ociClosureArtifactType) {
            debug("manifest has artifactType '%s', expected '%s'", artifactType, ociClosureArtifactType);
        }
    }

    if (!manifest.contains("layers") || !manifest["layers"].is_array()) {
        throw Error("OCI closure manifest has no layers array");
    }

    for (const auto & layer : manifest["layers"]) {
        // Required fields
        if (!layer.contains("digest"))
            throw Error("OCI layer missing digest");
        std::string digest = layer["digest"].get<std::string>();

        if (!layer.contains("size"))
            throw Error("OCI layer missing size");
        uint64_t size = layer["size"].get<uint64_t>();

        // Layer annotations contain metadata
        const auto & annotations = layer.value("annotations", nlohmann::json::object());

        if (!annotations.contains(annotationStorePath))
            throw Error("OCI layer missing required annotation '%s'", annotationStorePath);
        StorePath storePath = parseStorePath(annotations[annotationStorePath].get<std::string>());

        if (!annotations.contains(annotationTarHash))
            throw Error(
                "OCI layer missing required annotation '%s' for path %s", annotationTarHash, printStorePath(storePath));
        Hash tarHash = Hash::parseAny(annotations[annotationTarHash].get<std::string>(), HashAlgorithm::SHA256);

        // Build LayerInfo with required fields initialized
        LayerInfo info{
            .digest = std::move(digest),
            .size = size,
            .storePath = std::move(storePath),
            .tarHash = std::move(tarHash),
            .references = {},
            .deriver = std::nullopt,
            .signatures = {}};

        // Optional: references
        if (annotations.contains(annotationReferences)) {
            auto refsStr = annotations[annotationReferences].get<std::string>();
            if (!refsStr.empty()) {
                for (const auto & ref : tokenizeString<std::vector<std::string>>(refsStr, " ")) {
                    info.references.insert(parseStorePath(ref));
                }
            }
        }

        // Optional: deriver
        if (annotations.contains(annotationDeriver)) {
            auto deriverStr = annotations[annotationDeriver].get<std::string>();
            if (!deriverStr.empty()) {
                info.deriver = parseStorePath(deriverStr);
            }
        }

        // Optional: signatures
        if (annotations.contains(annotationSignatures)) {
            auto sigsStr = annotations[annotationSignatures].get<std::string>();
            for (const auto & sig : tokenizeString<std::vector<std::string>>(sigsStr, " ")) {
                info.signatures.insert(sig);
            }
        }

        layers.push_back(std::move(info));
    }

    return layers;
}

StorePathSet OciBinaryCacheStore::queryClosurePaths(const std::string & tag)
{
    auto manifestJson = getManifest(tag);
    if (!manifestJson)
        throw Error("closure manifest '%s' not found in OCI registry", tag);

    auto manifest = nlohmann::json::parse(*manifestJson);
    auto layers = parseClosureManifest(manifest);

    StorePathSet paths;
    for (const auto & layer : layers) {
        paths.insert(layer.storePath);
    }
    return paths;
}

void OciBinaryCacheStore::downloadAndExtractTarLayer(const LayerInfo & layer, Store & destStore, RepairFlag repair)
{
    // Skip if path already exists (unless repair mode)
    if (!repair && destStore.isValidPath(layer.storePath))
        return;

    // Download tar while computing hash
    StringSink tarSink;
    pullBlob(layer.digest, tarSink);

    // Verify the tar hash matches
    auto actualHash = hashString(HashAlgorithm::SHA256, tarSink.s);
    if (actualHash != layer.tarHash) {
        throw Error(
            "tar hash mismatch for %s: expected %s, got %s",
            printStorePath(layer.storePath),
            layer.tarHash.to_string(HashFormat::SRI, true),
            actualHash.to_string(HashFormat::SRI, true));
    }

    // Extract tar to temp directory
    auto tempDir = createTempDir();
    AutoDelete delTempDir(tempDir, true);

    StringSource tarSource(tarSink.s);
    unpackTarfile(tarSource, tempDir);

    // The tar extracted files to tempDir/nix/store/xxx-name/
    auto extractedPath = tempDir + storeDir + "/" + std::string(layer.storePath.to_string());

    // Compute NAR of the extracted content and add to store
    StringSink narSink;
    dumpPath(extractedPath, narSink);

    // Compute NAR hash for store registration
    auto narHash = hashString(HashAlgorithm::SHA256, narSink.s);

    // Create ValidPathInfo with metadata
    UnkeyedValidPathInfo unkeyedInfo{storeDir, narHash};
    unkeyedInfo.narSize = narSink.s.size();
    unkeyedInfo.references = layer.references;
    unkeyedInfo.deriver = layer.deriver;
    unkeyedInfo.sigs = layer.signatures;

    ValidPathInfo info{layer.storePath, std::move(unkeyedInfo)};

    // Add to destination store using NAR format
    StringSource narSource(narSink.s);
    destStore.addToStore(info, narSource, repair, NoCheckSigs);

    printInfo("installed %s (%d bytes)", printStorePath(layer.storePath), tarSink.s.size());
}

StorePath OciBinaryCacheStore::fetchClosure(const std::string & tag, Store & destStore, RepairFlag repair)
{
    Activity act(
        *logger,
        lvlInfo,
        actUnknown,
        fmt("fetching closure '%s' from OCI registry '%s'", tag, config->getHumanReadableURI()));

    // Fetch and parse the closure manifest
    auto manifestJson = getManifest(tag);
    if (!manifestJson)
        throw Error("closure manifest '%s' not found in OCI registry", tag);

    auto manifest = nlohmann::json::parse(*manifestJson);
    auto layers = parseClosureManifest(manifest);

    if (layers.empty())
        throw Error("closure manifest '%s' contains no layers", tag);

    // Get the root path from manifest annotation
    StorePath rootPath = layers.back().storePath; // Default: last layer
    const auto & manifestAnnotations = manifest.value("annotations", nlohmann::json::object());
    if (manifestAnnotations.contains(annotationRootPath)) {
        rootPath = parseStorePath(manifestAnnotations[annotationRootPath].get<std::string>());
    }

    // Identify which layers need to be downloaded (not already in destination store)
    std::vector<const LayerInfo *> missingLayers;
    for (const auto & layer : layers) {
        if (!destStore.isValidPath(layer.storePath) || repair) {
            missingLayers.push_back(&layer);
        }
    }

    if (missingLayers.empty()) {
        printInfo("all %d paths already present in store", layers.size());
        return rootPath;
    }

    printInfo("downloading %d/%d missing paths from closure", missingLayers.size(), layers.size());

    // Download and extract layers (sequentially for now - tar extraction needs care)
    for (const auto * layer : missingLayers) {
        checkInterrupt();

        debug("downloading layer %s", printStorePath(layer->storePath));
        downloadAndExtractTarLayer(*layer, destStore, repair);
    }

    return rootPath;
}

// ========== Push Support ==========

bool OciBinaryCacheStore::blobExists(const std::string & digest)
{
    // Authenticate with push scope
    authenticate("repository:" + config->repository + ":pull,push");

    auto path = "/v2/" + config->repository + "/blobs/" + digest;
    auto request = makeRequest(path);
    request.method = HttpMethod::Head;

    try {
        getFileTransfer()->download(request);
        return true;
    } catch (FileTransferError & e) {
        if (e.error == FileTransfer::NotFound)
            return false;
        throw;
    }
}

void OciBinaryCacheStore::pushBlob(const std::string & data, const std::string & digest)
{
    // Check if blob already exists (deduplication)
    if (blobExists(digest)) {
        debug("blob %s already exists, skipping upload", digest);
        return;
    }

    // Authenticate with push scope
    authenticate("repository:" + config->repository + ":pull,push");

    // OCI Distribution API two-step upload:
    // 1. POST to initiate upload (returns 202 with Location header)
    // 2. PUT to complete upload with digest

    // Step 1: Initiate upload (POST with empty body)
    auto initPath = "/v2/" + config->repository + "/blobs/uploads/";
    auto initUrl = config->getRegistryBaseUrl() + initPath;

    FileTransferRequest initRequest(initUrl);
    initRequest.method = HttpMethod::Post;
    initRequest.headers.push_back({"Content-Length", "0"});

    // Add auth header
    {
        auto state(_state.lock());
        if (!state->authToken.empty()) {
            initRequest.headers.push_back({"Authorization", "Bearer " + state->authToken});
        }
    }

    // Need to provide empty data to satisfy FileTransferRequest assertions
    static std::string emptyData;
    StringSource emptySource(emptyData);
    initRequest.data = {0, emptySource};

    auto initResult = getFileTransfer()->upload(initRequest);

    // Get the upload URL from the Location header
    if (!initResult.locationHeader) {
        throw Error("OCI registry did not return Location header for upload initiation");
    }

    // The Location header may be relative or absolute
    std::string uploadUrl = *initResult.locationHeader;
    if (uploadUrl.find("://") == std::string::npos) {
        // Relative URL - prepend base URL
        if (uploadUrl[0] != '/') {
            uploadUrl = "/" + uploadUrl;
        }
        uploadUrl = config->getRegistryBaseUrl() + uploadUrl;
    }

    // Append digest to upload URL
    uploadUrl += (uploadUrl.find('?') != std::string::npos ? "&" : "?") + std::string("digest=") + percentEncode(digest);

    // Step 2: Complete upload (PUT with body)
    FileTransferRequest uploadRequest(uploadUrl);
    uploadRequest.method = HttpMethod::Put;
    uploadRequest.headers.push_back({"Content-Type", "application/octet-stream"});

    // Add auth header
    {
        auto state(_state.lock());
        if (!state->authToken.empty()) {
            uploadRequest.headers.push_back({"Authorization", "Bearer " + state->authToken});
        }
    }

    // Create a StringSource for the data
    StringSource dataSource(data);
    uploadRequest.data = {data.size(), dataSource};

    try {
        getFileTransfer()->upload(uploadRequest);
        debug("pushed blob %s (%d bytes)", digest, data.size());
    } catch (FileTransferError & e) {
        throw Error("failed to upload blob to OCI registry: %s", e.message());
    }
}

void OciBinaryCacheStore::pushManifest(const nlohmann::json & manifest, const std::string & reference)
{
    // Authenticate with push scope
    authenticate("repository:" + config->repository + ":pull,push");

    auto path = "/v2/" + config->repository + "/manifests/" + reference;
    auto url = config->getRegistryBaseUrl() + path;

    auto manifestStr = manifest.dump();

    FileTransferRequest request(url);
    request.method = HttpMethod::Put;
    request.headers.push_back({"Content-Type", ociManifestMediaType});

    // Add auth header
    {
        auto state(_state.lock());
        if (!state->authToken.empty()) {
            request.headers.push_back({"Authorization", "Bearer " + state->authToken});
        }
    }

    // Create a StringSource for the manifest data
    StringSource manifestSource(manifestStr);
    request.data = {manifestStr.size(), manifestSource};

    try {
        getFileTransfer()->upload(request);
        debug("pushed manifest to %s", reference);
    } catch (FileTransferError & e) {
        throw Error("failed to push manifest to OCI registry: %s", e.message());
    }
}

nlohmann::json
OciBinaryCacheStore::buildLayerDescriptor(const std::string & digest, uint64_t size, const ValidPathInfo & info, const Hash & tarHash)
{
    nlohmann::json layer = {
        {"mediaType", ociLayerMediaType},
        {"digest", digest},
        {"size", size},
        {"annotations",
         {{annotationStorePath, printStorePath(info.path)},
          {annotationTarHash, tarHash.to_string(HashFormat::SRI, true)}}}};

    // Add references (use basename format)
    std::string refs;
    for (const auto & ref : info.references) {
        if (!refs.empty())
            refs += " ";
        refs += std::string(ref.to_string());
    }
    layer["annotations"][annotationReferences] = refs;

    // Add optional fields
    if (info.deriver) {
        layer["annotations"][annotationDeriver] = std::string(info.deriver->to_string());
    }

    if (!info.sigs.empty()) {
        std::string sigs;
        for (const auto & sig : info.sigs) {
            if (!sigs.empty())
                sigs += " ";
            sigs += sig;
        }
        layer["annotations"][annotationSignatures] = sigs;
    }

    return layer;
}

nlohmann::json OciBinaryCacheStore::buildClosureManifest(
    const std::vector<StorePath> & paths,
    const std::map<StorePath, ref<const ValidPathInfo>> & pathInfos,
    const std::map<StorePath, std::tuple<std::string, uint64_t, Hash>> & layerDigests,
    const StorePath & rootPath)
{
    nlohmann::json layers = nlohmann::json::array();

    for (const auto & path : paths) {
        auto it = pathInfos.find(path);
        if (it == pathInfos.end())
            throw Error("missing path info for %s", printStorePath(path));

        auto digestIt = layerDigests.find(path);
        if (digestIt == layerDigests.end())
            throw Error("missing digest for %s", printStorePath(path));

        const auto & [digest, size, tarHash] = digestIt->second;
        layers.push_back(buildLayerDescriptor(digest, size, *it->second, tarHash));
    }

    // Build config blob (empty JSON object for now, just need a valid blob)
    nlohmann::json configBlob = {{"rootPath", printStorePath(rootPath)}, {"closureSize", paths.size()}};
    auto configData = configBlob.dump();
    auto configDigest = "sha256:" + hashString(HashAlgorithm::SHA256, configData).to_string(HashFormat::Base16, false);

    // Push config blob
    pushBlob(configData, configDigest);

    // Build manifest
    nlohmann::json manifest = {
        {"schemaVersion", 2},
        {"mediaType", ociManifestMediaType},
        {"artifactType", ociClosureArtifactType},
        {"config", {{"mediaType", ociConfigMediaType}, {"digest", configDigest}, {"size", configData.size()}}},
        {"layers", layers},
        {"annotations",
         {{annotationRootPath, printStorePath(rootPath)}, {annotationClosureSize, std::to_string(paths.size())}}}};

    return manifest;
}

void OciBinaryCacheStore::upsertFile(
    const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint)
{
    checkEnabled();

    // Read all data from source into a string
    StringSink sink;
    source.drainInto(sink);
    std::string data = std::move(sink.s);

    debug("OCI upsertFile: path=%s, size=%d", path, data.size());

    // If this is a .narinfo file, create a tar layer and push an OCI manifest
    if (hasSuffix(path, ".narinfo")) {
        // Parse the NarInfo to extract metadata
        auto narInfo = make_ref<NarInfo>(*this, data, path);

        // Get the real path to the store path
        auto realStorePath = storeDir + "/" + std::string(narInfo->path.to_string());

        // Create a tar archive from the store path
        StringSink tarSink;
        Hash tarHash = createStoreTar(realStorePath, tarSink);
        auto tarDigest = "sha256:" + tarHash.to_string(HashFormat::Base16, false);

        debug("OCI push: created tar for %s, size=%d, digest=%s",
              printStorePath(narInfo->path), tarSink.s.size(), tarDigest);

        // Push tar blob to registry
        pushBlob(tarSink.s, tarDigest);

        // Build layer descriptor with tar hash
        nlohmann::json layer = {
            {"mediaType", ociLayerMediaType},
            {"digest", tarDigest},
            {"size", tarSink.s.size()},
            {"annotations",
             {{annotationStorePath, printStorePath(narInfo->path)},
              {annotationTarHash, tarHash.to_string(HashFormat::SRI, true)}}}};

        // Add references
        std::string refs;
        for (const auto & ref : narInfo->references) {
            if (!refs.empty())
                refs += " ";
            refs += std::string(ref.to_string());
        }
        layer["annotations"][annotationReferences] = refs;

        // Add optional fields
        if (narInfo->deriver) {
            layer["annotations"][annotationDeriver] = std::string(narInfo->deriver->to_string());
        }

        if (!narInfo->sigs.empty()) {
            std::string sigs;
            for (const auto & sig : narInfo->sigs) {
                if (!sigs.empty())
                    sigs += " ";
                sigs += sig;
            }
            layer["annotations"][annotationSignatures] = sigs;
        }

        // Build config blob
        nlohmann::json configBlob = {{"storePath", printStorePath(narInfo->path)}};
        auto configData = configBlob.dump();
        auto configDigest = "sha256:" + hashString(HashAlgorithm::SHA256, configData).to_string(HashFormat::Base16, false);
        pushBlob(configData, configDigest);

        // Build manifest
        nlohmann::json manifestAnnotations = {
            {annotationStorePath, printStorePath(narInfo->path)},
            {annotationTarHash, tarHash.to_string(HashFormat::SRI, true)},
            {annotationRootPath, printStorePath(narInfo->path)},
            {annotationClosureSize, "1"},
            {annotationReferences, refs}};

        if (narInfo->deriver) {
            manifestAnnotations[annotationDeriver] = std::string(narInfo->deriver->to_string());
        }

        if (!narInfo->sigs.empty()) {
            std::string sigs;
            for (const auto & sig : narInfo->sigs) {
                if (!sigs.empty())
                    sigs += " ";
                sigs += sig;
            }
            manifestAnnotations[annotationSignatures] = sigs;
        }

        nlohmann::json manifest = {
            {"schemaVersion", 2},
            {"mediaType", ociManifestMediaType},
            {"artifactType", ociClosureArtifactType},
            {"config", {{"mediaType", ociConfigMediaType}, {"digest", configDigest}, {"size", configData.size()}}},
            {"layers", nlohmann::json::array({layer})},
            {"annotations", manifestAnnotations}};

        debug("OCI push: manifest layers[0].digest=%s, config.digest=%s",
              manifest["layers"][0]["digest"].get<std::string>(),
              manifest["config"]["digest"].get<std::string>());

        // Extract store path hash for tag
        auto tag = pathToTag(path);
        pushManifest(manifest, tag);
    }
    // Ignore NAR files - we create tar layers directly from store paths
}

std::optional<std::string> OciBinaryCacheStore::getNixCacheInfo()
{
    // OCI stores don't use nix-cache-info
    // Return a synthetic one
    return "StoreDir: " + storeDir + "\n";
}

std::optional<TrustedFlag> OciBinaryCacheStore::isTrustedClient()
{
    // OCI stores are typically read-only and don't have a notion of trust
    return std::nullopt;
}

ref<Store> OciBinaryCacheStoreConfig::openStore() const
{
    return make_ref<OciBinaryCacheStore>(ref{std::const_pointer_cast<OciBinaryCacheStoreConfig>(shared_from_this())});
}

static RegisterStoreImplementation<OciBinaryCacheStoreConfig> regOciBinaryCacheStore;

} // namespace nix
