#!/usr/bin/env bash
# Test audit logging functionality in nix-daemon

source common.sh

# Only run if we have a daemon
requireDaemonNewerThan "2.30"

# Test that audit logging configuration option exists and defaults to false
nix config show | grep -q "enable-daemon-audit-log = false" || fail "enable-daemon-audit-log config option not found or has wrong default"

# Note: We can't easily test the actual audit logging in functional tests
# because we would need to:
# 1. Start our own daemon with specific config
# 2. Capture its stderr output
# 3. Parse the audit logs
# 
# This is better tested manually or in a more integrated test environment
# where we can control the daemon startup and configuration.

echo "Audit logging configuration test passed"