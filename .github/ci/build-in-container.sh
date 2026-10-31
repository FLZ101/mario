#!/bin/sh

set -eu

flags=${1:-}

. /etc/os-release

if [ "$ID" = ubuntu ]; then
    if [ "$VERSION_ID" = "23.10" ]; then
        sed -i \
            -e 's|http://archive.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g' \
            -e 's|http://security.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g' \
            /etc/apt/sources.list /etc/apt/sources.list.d/*.sources 2>/dev/null || true
    fi
    apt-get update
    packages=$(grep -Ev '^[[:space:]]*#|^[[:space:]]*$' .github/ci/ubuntu-build-packages.txt)
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $packages
else
    if [ "$VERSION_ID" = "39" ]; then
        sed -i \
            -e 's|^metalink=|#metalink=|' \
            -e 's|^#baseurl=http://download.example/pub/fedora/linux|baseurl=https://archives.fedoraproject.org/pub/archive/fedora/linux|' \
            /etc/yum.repos.d/fedora.repo /etc/yum.repos.d/fedora-updates.repo
    fi
    packages=$(grep -Ev '^[[:space:]]*#|^[[:space:]]*$' .github/ci/fedora-packages.txt)
    dnf install -y --setopt=install_weak_deps=False $packages
fi

make kernel app $flags
