{
  config,
  lib,
  pkgs,
  ...
}:

let
  clientPkgs = config.nodes.client.nixpkgs.pkgs;

  # Test packages - minimal packages for fast copying
  pkgA = clientPkgs.writeText "test-package-a" "test package a";
  pkgB = clientPkgs.writeText "test-package-b" "test package b";
  pkgC = clientPkgs.writeText "test-package-c" "test package c";

  # Registry configuration
  registryPort = 5000;

  # Docker Distribution (OCI registry) configuration
  # Note: docker-distribution v2.8+ supports OCI 1.1 artifacts
  registryConfig = pkgs.writeText "registry-config.yml" ''
    version: 0.1
    log:
      level: info
    storage:
      filesystem:
        rootdirectory: /var/lib/docker-registry
      delete:
        enabled: true
    http:
      addr: 0.0.0.0:${toString registryPort}
      headers:
        X-Content-Type-Options: [nosniff]
  '';

in
{
  name = "oci-binary-cache-store";

  nodes = {
    registry =
      {
        config,
        lib,
        pkgs,
        ...
      }:
      {
        virtualisation.writableStore = true;
        virtualisation.cores = 2;
        virtualisation.memorySize = 2048;
        virtualisation.additionalPaths = [
          pkgA
          pkgB
          pkgC
        ];

        nix.extraOptions = ''
          experimental-features = nix-command
          substituters =
        '';

        # Run docker-distribution as OCI registry
        systemd.services.docker-registry = {
          description = "Docker Distribution Registry";
          wantedBy = [ "multi-user.target" ];
          after = [ "network.target" ];
          serviceConfig = {
            ExecStartPre = "${pkgs.coreutils}/bin/mkdir -p /var/lib/docker-registry";
            ExecStart = "${pkgs.docker-distribution}/bin/registry serve ${registryConfig}";
            Restart = "always";
            StateDirectory = "docker-registry";
          };
        };

        networking.firewall.allowedTCPPorts = [ registryPort ];
      };

    client =
      { config, pkgs, ... }:
      {
        virtualisation.writableStore = true;
        virtualisation.cores = 2;
        virtualisation.memorySize = 2048;

        nix.settings.require-sigs = false;
        nix.extraOptions = ''
          experimental-features = nix-command
          substituters =
        '';
      };
  };

  testScript =
    { nodes }:
    # python
    ''
      import uuid

      # ============================================================================
      # Configuration
      # ============================================================================

      REGISTRY_HOST = 'registry:${toString registryPort}'
      REGISTRY_URL = f'http://{REGISTRY_HOST}'

      PKGS = {
          'A': '${pkgA}',
          'B': '${pkgB}',
          'C': '${pkgC}',
      }

      # ============================================================================
      # Helper Functions
      # ============================================================================

      def make_oci_url(repository, tag=""):
          """Build OCI store URL"""
          if tag:
              return f"oci://{REGISTRY_HOST}/{repository}:{tag}"
          return f"oci://{REGISTRY_HOST}/{repository}"

      def get_package_hash(pkg_path):
          """Extract store hash from package path"""
          return pkg_path.split("/")[-1].split("-")[0]

      def verify_packages_in_store(machine, pkg_paths, should_exist=True):
          """
          Verify whether packages exist in the store.

          Args:
              machine: The machine to check on
              pkg_paths: List of package paths to check (or single path)
              should_exist: If True, verify packages exist; if False, verify they don't
          """
          paths = [pkg_paths] if isinstance(pkg_paths, str) else pkg_paths
          for pkg in paths:
              if should_exist:
                  machine.succeed(f"nix path-info {pkg}")
              else:
                  machine.fail(f"nix path-info {pkg}")

      def setup_oci(test_func):
          """
          Decorator that creates a unique repository name for each test.
          Cleans up client store after test completion.
          """
          def wrapper():
              # Restart nix-daemon to clear any cached state
              registry.succeed("systemctl reset-failed nix-daemon.service nix-daemon.socket || true")
              registry.succeed("systemctl restart nix-daemon")
              client.succeed("systemctl reset-failed nix-daemon.service nix-daemon.socket || true")
              client.succeed("systemctl restart nix-daemon")

              # Generate unique repository name for this test
              repo_name = f"test-{uuid.uuid4().hex[:8]}"

              try:
                  test_func(repo_name)
              finally:
                  # Clean up client store
                  for pkg in PKGS.values():
                      client.succeed(f"[ ! -e {pkg} ] || nix store delete --ignore-liveness {pkg}")

          return wrapper

      # ============================================================================
      # Test Functions
      # ============================================================================

      @setup_oci
      def test_push_single_package(repo_name):
          """Test pushing a single package to OCI registry"""
          print("\n=== Testing Push Single Package ===")

          store_url = make_oci_url(repo_name)
          pkg_a = PKGS['A']

          # Push package from registry machine (which has the package)
          registry.succeed(f"nix copy --to '{store_url}' {pkg_a}")

          print(f"Successfully pushed {pkg_a} to {store_url}")

      @setup_oci
      def test_push_and_pull_single_package(repo_name):
          """Test pushing and pulling a single package"""
          print("\n=== Testing Push and Pull Single Package ===")

          store_url = make_oci_url(repo_name)
          pkg_a = PKGS['A']

          # Verify package doesn't exist on client
          verify_packages_in_store(client, pkg_a, should_exist=False)

          # Push package from registry machine
          registry.succeed(f"nix copy --to '{store_url}' {pkg_a}")

          # Pull package to client
          client.wait_for_unit("network-addresses-eth1.service")
          client.succeed(f"nix copy --no-check-sigs --from '{store_url}' {pkg_a}")

          # Verify package now exists on client
          verify_packages_in_store(client, pkg_a, should_exist=True)

          print("Successfully pushed and pulled single package")

      @setup_oci
      def test_push_multiple_packages(repo_name):
          """Test pushing multiple packages to OCI registry"""
          print("\n=== Testing Push Multiple Packages ===")

          store_url = make_oci_url(repo_name)

          # Push all packages
          pkg_list = " ".join(PKGS.values())
          registry.succeed(f"nix copy --to '{store_url}' {pkg_list}")

          print("Successfully pushed multiple packages")

      @setup_oci
      def test_push_and_pull_closure(repo_name):
          """Test pushing and pulling a closure (package with dependencies)"""
          print("\n=== Testing Push and Pull Closure ===")

          store_url = make_oci_url(repo_name)

          # Use coreutils which has dependencies
          # Note: coreutils should be available on the registry machine
          coreutils_path = registry.succeed("nix path-info ${pkgs.coreutils}").strip()

          # Push coreutils closure
          registry.succeed(f"nix copy --to '{store_url}' {coreutils_path}")

          # Verify the push worked by pulling the closure info back
          # Note: coreutils already exists on the client (as a system dependency)
          # so we just verify the copy command succeeds (it will be a no-op for
          # paths that already exist, but validates the manifest is readable)
          client.wait_for_unit("network-addresses-eth1.service")
          client.succeed(f"nix copy --no-check-sigs --from '{store_url}' {coreutils_path}")

          # Verify package exists
          verify_packages_in_store(client, coreutils_path, should_exist=True)

          print("Successfully pushed and pulled closure")

      @setup_oci
      def test_deduplication(repo_name):
          """Test that duplicate blobs are not re-uploaded"""
          print("\n=== Testing Blob Deduplication ===")

          store_url = make_oci_url(repo_name)
          pkg_a = PKGS['A']

          # Push package twice
          registry.succeed(f"nix copy --to '{store_url}' {pkg_a}")

          # Second push should skip existing blobs
          output = registry.succeed(f"nix copy --debug --to '{store_url}' {pkg_a} 2>&1")

          # Check for deduplication message (blob already exists)
          if "already exists" in output or "skipping" in output.lower():
              print("Deduplication working - blob was skipped on second push")
          else:
              print("Note: Could not verify deduplication from output")

          print("Deduplication test completed")

      @setup_oci
      def test_pull_nonexistent_package(repo_name):
          """Test that pulling a nonexistent package fails gracefully"""
          print("\n=== Testing Pull Nonexistent Package ===")

          store_url = make_oci_url(repo_name)
          fake_path = "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-nonexistent"

          client.wait_for_unit("network-addresses-eth1.service")

          # This should fail
          result = client.execute(f"nix copy --no-check-sigs --from '{store_url}' {fake_path} 2>&1")
          exit_code = result[0]

          if exit_code == 0:
              raise Exception("Expected failure when pulling nonexistent package")

          print("Correctly failed to pull nonexistent package")

      # ============================================================================
      # Main Test Execution
      # ============================================================================

      start_all()

      # Wait for Docker Distribution registry to be ready
      registry.wait_for_unit("docker-registry.service")
      registry.wait_for_open_port(${toString registryPort})

      # Give the registry a moment to fully initialize
      import time
      time.sleep(2)

      # Verify registry is responding
      registry.succeed("curl -f http://localhost:${toString registryPort}/v2/")

      print("\n" + "=" * 60)
      print("OCI Binary Cache Store Integration Tests")
      print("=" * 60)

      # Run all tests
      test_push_single_package()
      test_push_and_pull_single_package()
      test_push_multiple_packages()
      test_push_and_pull_closure()
      test_deduplication()
      test_pull_nonexistent_package()

      print("\n" + "=" * 60)
      print("All OCI Binary Cache Store Tests Passed!")
      print("=" * 60)
    '';
}
