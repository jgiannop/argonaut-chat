#!/usr/bin/env bash
# Builds the app's .deb for each Ubuntu release, each in a clean container of
# that release (a package is tied to its release's Qt), then installs it on
# a fresh container of the same release to check that it installs, finds its
# libraries and starts.
#
#   packaging/build-debs.sh               # 24.04 and 26.04
#   packaging/build-debs.sh 26.04         # one release
#
# The packages land in dist/ (not kept in git):
#   dist/argonaut-chat_<version>_ubuntu<release>_<arch>.deb
# Needs Docker. Downloads Ubuntu's images and packages, nothing else.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$SRC/dist"
RELEASES=("$@")
[[ ${#RELEASES[@]} -gt 0 ]] || RELEASES=(24.04 26.04)

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31mError: %s\033[0m\n' "$*" >&2; exit 1; }

command -v docker >/dev/null || die "Docker is needed"
mkdir -p "$DIST"

for release in "${RELEASES[@]}"; do
    [[ $release =~ ^[0-9]{2}\.[0-9]{2}$ ]] || die "not a release: $release"
    say "Ubuntu $release: building"
    docker run --rm -v "$SRC":/src:ro -v "$DIST":/dist -e RELEASE="$release" -e OWNER="$(id -u):$(id -g)" \
        "ubuntu:$release" bash -euo pipefail -c '
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -q >/dev/null
            apt-get install -y -q --no-install-recommends build-essential cmake ninja-build pkg-config \
                dpkg-dev file qt6-base-dev qt6-websockets-dev libsodium-dev libsqlcipher-dev qtkeychain-qt6-dev \
                >/dev/null
            cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
                -DBUILD_TESTING=OFF -DCHAT_PACKAGE=ON >/dev/null
            cmake --build /build
            cd /build && cpack --config CPackConfig.cmake >/dev/null
            deb=$(ls /build/*.deb)
            name=$(basename "$deb" | sed "s/_\([a-z0-9]*\)\.deb$/_ubuntu${RELEASE}_\1.deb/")
            cp "$deb" "/dist/$name"
            chown "$OWNER" "/dist/$name"
            echo "built $name"
        '

    say "Ubuntu $release: installing it on a fresh system"
    deb=$(ls -t "$DIST"/argonaut-chat_*_ubuntu"$release"_*.deb | head -1)
    docker run --rm -v "$DIST":/dist:ro -e DEB="/dist/$(basename "$deb")" "ubuntu:$release" bash -euo pipefail -c '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -q >/dev/null
        apt-get install -y -q "$DEB" >/dev/null
        argonaut-chat --version
        if ldd /usr/bin/argonaut-chat | grep "not found"; then echo "missing libraries"; exit 1; fi
        test -f /usr/share/applications/argonaut-chat.desktop
        test -f /usr/share/icons/hicolor/scalable/apps/argonaut-chat.svg
        # It starts: without a keyring here it shows that it has none and waits.
        set +e
        QT_QPA_PLATFORM=offscreen timeout 5 argonaut-chat >/tmp/out 2>&1
        code=$?
        set -e
        if [[ $code != 124 ]]; then echo "it did not start (exit $code):"; cat /tmp/out; exit 1; fi
        echo "installs, finds its libraries, and starts"
    '
done

say "Packages"
ls -l "$DIST"/*.deb
