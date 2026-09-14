#!/bin/bash
#
# Package the already-built mkblob/mkblobid binaries as .rpm and/or .deb.
# Run ./makeall.sh first -- this script only packages, it does not build.
#
# .deb is built natively with dpkg-deb (no alien, no debhelper needed).
#
# (c) 2026 Sigurd Dagestad

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$SCRIPT_DIR"

VERSION="$(cat "$SCRIPT_DIR/VERSION" 2>/dev/null || echo 2.1)"
RELEASE="1"
ARCH="x86_64"
DEB_ARCH="amd64"
DIST_DIR="$SCRIPT_DIR/dist"

DO_RPM=0
DO_DEB=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--rpm] [--deb] [--all] [-h|--help]

  --rpm   Build an .rpm package (needs rpmbuild: dnf install rpm-build).
  --deb   Build a .deb package directly with dpkg-deb, no alien needed
          (needs dpkg: dnf install dpkg).
  --all   Build both. Default if no flag is given.

Packages are written to ./dist/. Run ./makeall.sh first so mkblob
and mkblobid exist -- this script only packages already-built binaries.
EOF
}

if [ $# -eq 0 ]; then
    DO_RPM=1
    DO_DEB=1
fi
while [ $# -gt 0 ]; do
    case "$1" in
        --rpm) DO_RPM=1 ;;
        --deb) DO_DEB=1 ;;
        --all) DO_RPM=1; DO_DEB=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

for f in mkblob mkblobid; do
    [ -x "$f" ] || {
        echo "error: $f not found or not executable -- run ./makeall.sh first" >&2
        exit 1
    }
done

mkdir -p "$DIST_DIR"

build_rpm() {
    echo "== Building RPM =="
    command -v rpmbuild >/dev/null ||
        { echo "error: rpmbuild not found (dnf install rpm-build)" >&2; exit 1; }

    local topdir="$DIST_DIR/rpmbuild"
    mkdir -p "$topdir"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

    # Remove any previously built rpm so a stale/differently-named one can't
    # linger next to the fresh build (the release script picks by glob).
    rm -f "$DIST_DIR/$ARCH"/*.rpm

    # Release carries no dist tag (see mkblob.spec) -- the package is a
    # distro-agnostic static binary. Only the version is passed in.
    rpmbuild -bb \
        --define "_topdir $topdir" \
        --define "_sourcedir $SCRIPT_DIR" \
        --define "_rpmdir $DIST_DIR" \
        --define "version $VERSION" \
        mkblob.spec

    echo "RPM written under $DIST_DIR/$ARCH/"
}

build_deb() {
    echo "== Building DEB (native dpkg-deb, no alien) =="
    command -v dpkg-deb >/dev/null ||
        { echo "error: dpkg-deb not found (dnf install dpkg)" >&2; exit 1; }

    local pkg="mkblob_${VERSION}-${RELEASE}_${DEB_ARCH}"
    local stage
    stage="$(mktemp -d)"
    trap 'rm -rf "$stage"' RETURN

    mkdir -p "$stage/DEBIAN" "$stage/usr/bin" "$stage/usr/share/doc/mkblob"

    install -m755 mkblob "$stage/usr/bin/mkblob"
    install -m755 mkblobid "$stage/usr/bin/mkblobid"
    install -m644 readme.txt README.md LICENSE "$stage/usr/share/doc/mkblob/"

    local size_kb
    size_kb=$(du -sk "$stage/usr" | cut -f1)

    cat > "$stage/DEBIAN/control" <<EOF
Package: mkblob
Version: ${VERSION}-${RELEASE}
Section: utils
Priority: optional
Architecture: ${DEB_ARCH}
Installed-Size: ${size_kb}
Maintainer: Sigurd Dagestad <sigurd@dagestad.info>
Homepage: https://github.com/sigurd-dev/mkblob
Description: Make library independent binary executables
 mkblob bundles an executable together with every shared library and
 extra file it needs, producing a single self-contained binary that
 runs on other Linux distributions without rebuilding or reinstalling
 dependencies. Can optionally lock the produced binary to a password, a
 specific machine (MAC address and/or hardware/OS UUID), and/or an
 expiry date.
 .
 Includes mkblobid, a helper that reports a machine's MAC address(es)
 and UUID for use with mkblob's -mac/-uuid flags.
EOF

    find "$stage" -type d -exec chmod 755 {} +
    dpkg-deb --root-owner-group --build "$stage" "$DIST_DIR/${pkg}.deb"
    echo "Wrote $DIST_DIR/${pkg}.deb"
}

[ "$DO_RPM" -eq 1 ] && build_rpm
[ "$DO_DEB" -eq 1 ] && build_deb

echo "Done."
