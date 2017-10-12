#!/usr/bin/env bash
set -euxo pipefail

if [ -f /Library/LaunchDaemons/org.nixos.nix-daemon.plist ]; then
    sudo launchctl unload /Library/LaunchDaemons/org.nixos.nix-daemon.plist
    sudo rm /Library/LaunchDaemons/org.nixos.nix-daemon.plist
fi

# FIXME: this assumes no further changes have happened to /etc/profile since
# the installation
if [ -f /etc/profile.backup-before-nix ]; then
    sudo mv /etc/profile.backup-before-nix /etc/profile
fi

if [ -f /etc/bashrc.backup-before-nix ]; then
    sudo mv /etc/bashrc.backup-before-nix /etc/bashrc
fi

if [ -f /etc/zshrc.backup-before-nix ]; then
    sudo mv /etc/zshrc.backup-before-nix /etc/zshrc
fi

for i in $(seq 1 $(sysctl -n hw.ncpu)); do
    if id -u "nixbld$i" ; then
        sudo /usr/bin/dscl . -delete "/Users/nixbld$i"
    fi
done

if id -g nixbld; then
    sudo /usr/bin/dscl . -delete "/Groups/nixbld"
fi

## Purge
sudo rm -rf /nix

