R"(

**Store URL format**: `oci://`*registry*`/`*repository*[`:`*tag*]

This store allows Nix to fetch and push store paths to OCI 1.1-compliant
container registries. Full closures are stored as multi-layer OCI images,
with each store path as a separate layer. This enables parallel transfers
and cross-closure deduplication.

**Requires OCI 1.1**: Uses `artifactType` and layer annotations.

**Compatible registries**: Harbor v2.12+, Amazon ECR, Azure ACR, Zot.

**Not compatible**: Docker Hub, ghcr.io (no OCI 1.1 support yet).

### URL Format

```
oci://registry.example.com/repository[:tag]
```

Examples:
- `oci://harbor.example.com/nix-cache:hello-closure` - Harbor with closure tag
- `oci://ecr.aws/myorg/nix-cache` - Amazon ECR
- `oci://myregistry.azurecr.io/nix-cache` - Azure Container Registry
- `oci://localhost:5000/nix-cache` - Local Zot registry

### Closure Format (Multi-Layer)

Closures are stored as OCI images where each layer contains a single store
path's NAR data:

```
registry/repo:hello-closure
  manifest.json
    artifactType: "application/vnd.nix.closure.v1"
    layers[]:
      layer[0]: glibc NAR     (digest: sha256:aaa...)
      layer[1]: gcc-libs NAR  (digest: sha256:bbb...)
      layer[2]: hello NAR     (digest: sha256:ccc...)
```

Layer annotations contain NarInfo metadata:
- `org.nixos.store-path`: The full store path
- `org.nixos.nar-hash`: The NAR hash for verification
- `org.nixos.references`: Space-separated list of dependencies
- `org.nixos.deriver`: The deriver path (if known)
- `org.nixos.signatures`: Space-separated list of signatures

Manifest-level annotations:
- `org.nixos.root-path`: The root store path of the closure
- `org.nixos.closure-size`: Number of paths in the closure

### Layer Deduplication

Since layers are content-addressed by OCI digest, shared dependencies are
stored only once in the registry:

- `hello` and `curl` both depend on `glibc`
- `glibc` NAR has digest `sha256:xyz...`
- Both closure manifests reference the same blob
- Registry stores it once, serves it to both
- Downloads are deduplicated automatically

### Authentication

Authentication is handled via `~/.docker/config.json`, which is compatible
with `docker login`, `podman login`, and credential helpers.

To authenticate with a registry:

```bash
# Harbor
docker login harbor.example.com

# Amazon ECR
aws ecr get-login-password | docker login --username AWS --password-stdin 123456789.dkr.ecr.us-east-1.amazonaws.com

# Azure ACR
az acr login --name myregistry
```

### Usage

#### As a Substituter

Add the OCI store as a substituter in your Nix configuration:

```nix
# In nix.conf
substituters = oci://harbor.example.com/nix-cache https://cache.nixos.org
trusted-public-keys = cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY=
```

Or use it for a single command:

```bash
nix build --substituters 'oci://harbor.example.com/nix-cache' nixpkgs#hello
```

#### Pushing to a Registry

Push store paths to an OCI registry:

```bash
# Push a single store path and its closure
nix copy --to 'oci://harbor.example.com/nix-cache' /nix/store/abc123-hello-2.10

# Push from nixpkgs
nix copy --to 'oci://harbor.example.com/nix-cache' nixpkgs#hello
```

When pushing, Nix will:

1. Compute the closure of the store path
2. Check which blobs already exist in the registry (deduplication)
3. Upload missing NAR blobs in parallel
4. Create and push a closure manifest with layer annotations

### Parallel Transfers

Both downloads and uploads use parallel connections for efficiency. Missing
layers are transferred concurrently using multiple HTTP connections. The
number of connections is controlled by the `http-connections` setting
(default: 25).

### OCI 1.1 Features Used

1. **`artifactType`**: Identifies Nix closures in mixed registries
   - `application/vnd.nix.closure.v1` for closure manifests

2. **Layer annotations**: Per-layer NarInfo metadata
   - Enables verification during download
   - Supports incremental closure fetching

3. **Content-addressable blobs**: Automatic deduplication
   - Shared dependencies stored once
   - Cross-closure blob sharing

### Limitations

- **OCI 1.1 required**: Does not work with registries that lack OCI 1.1 support.
- **No garbage collection**: The store does not manage garbage collection of
  images in the registry.
- **Push permissions**: Pushing requires write access to the registry repository.

### Troubleshooting

If you encounter authentication issues:

1. Verify your credentials work with `docker pull`:
   ```bash
   docker pull harbor.example.com/nix-cache:some-tag
   ```

2. Check that `~/.docker/config.json` contains valid credentials for your registry.

3. For OCI 1.1 compatibility issues, verify your registry version:
   - Harbor: v2.12 or later
   - Amazon ECR: Supported
   - Azure ACR: Supported (public preview)
   - Zot: Supported

)"
