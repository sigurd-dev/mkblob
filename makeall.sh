#!/bin/bash
#
# Build mkblob and its embedded CDE-based runtime environment.
#
# Targets x86_64 CentOS/RHEL 8. Needs: sudo dnf install glibc-static
#
# (c) 2020 Sigurd Dagestad

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$SCRIPT_DIR"

# Single source of truth for the version (also read by build-packages.sh
# and passed to mkblob.spec). Compiled into the mkblob binary so
# `mkblob -version` reports it.
VERSION="$(cat "$SCRIPT_DIR/VERSION" 2>/dev/null || echo dev)"

DO_CLEAN=0
DO_UNPACK=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--clean] [--unpack] [-h|--help]

  --clean   Remove previous build artifacts (blob/, blob.tar.gz, cde)
            before building.
  --unpack  Unpack CDE.tar.gz before building (needed on a fresh checkout;
            skip on repeat builds that already have a ./CDE directory).
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --clean) DO_CLEAN=1 ;;
        --unpack) DO_UNPACK=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

step_clean() {
    [ "$DO_CLEAN" -eq 1 ] || return 0
    echo "== Removing previous build artifacts =="
    rm -rf blob blob.tar.gz cde
}

step_unpack_cde() {
    [ "$DO_UNPACK" -eq 1 ] || return 0
    echo "== Unpacking CDE.tar.gz =="
    tar -zxf CDE.tar.gz
}

# Build a static cde/cde-exec from the bundled CDE/strace-4.6 sources.
step_build_cde_static() {
    echo "== Building static cde/cde-exec =="
    (
        cd CDE/readelf-mini
        make
    )
    (
        cd CDE/strace-4.6
        ./configure CFLAGS="-g -O2 -std=gnu89 -fcommon" LDFLAGS="-static"
        make
    )
    cp CDE/strace-4.6/cde .
}

# Build a static standalone okapi (the file-copy helper mkblob shells out to).
step_build_okapi() {
    echo "== Building static okapi =="
    gcc -Wall -g -O2 -D_GNU_SOURCE -DOKAPI_STANDALONE \
        CDE/strace-4.6/okapi.c -o okapi -static
    strip okapi
}

# Stage the files that make up the embedded runtime environment.
step_stage_blob_files() {
    echo "== Staging blob/ =="
    # blob/ is a regenerated staging area (cde/okapi copies + trace output),
    # never hand-edited -- always start it fresh so re-running this script
    # is idempotent instead of colliding with a previous run's cde-root.
    rm -rf blob
    mkdir blob
    cp cde okapi mkblob.c mkblobexec.c blobgz.S sha256.h blob/
    strip blob/cde blob/okapi
}

# Use the "cde" tracer against a nonexistent tull.c to capture every file
# gcc/cc1/as/collect2/ld touch, building up cde-package/cde-root as a
# self-contained copy of the toolchain. These invocations are expected to
# fail (there's no tull.c to compile) -- only the side effect of having
# been traced matters, so failures here are deliberately ignored.
step_bootstrap_cde_package() {
    echo "== Tracing the toolchain into cde-package/ (errors below are expected) =="
    (
        cd blob
        ./cde /usr/bin/gcc tull.c || true
        cp ../templates/* ./cde-package
        ./cde /usr/libexec/gcc/x86_64-redhat-linux/8/cc1 tull.c || true
        ./cde /usr/bin/as tull.c || true
        ./cde /usr/libexec/gcc/x86_64-redhat-linux/8/collect2 tull.c || true
        ./cde /usr/bin/ld tull.c || true

        # Trace real compiles of mkblob/mkblobexec too, so cde-root ends up
        # with every static lib (libc.a, libpthread.a, ...) gcc needs for
        # both static and dynamic, pthread and non-pthread linking. The
        # resulting mkblob/mkblobexec.o files themselves are throwaway --
        # the real ones are built later, outside of cde.
        ./cde gcc -c mkblobexec.c blobgz.S -o mkblobexec.o || true
        ./cde gcc mkblob.c -o mkblob -lpthread -static || true
        ./cde gcc mkblob.c -o mkblob || true
        ./cde gcc mkblob.c -o mkblob -lpthread || true
        ./cde gcc mkblob.c -o mkblob -lpthread -static || true
    )
}

# Flatten cde-package/ into blob/ (drop the wrapper directory CDE adds).
step_flatten_cde_package() {
    echo "== Flattening cde-package/ into blob/ =="
    rm -rf blob/cde-package/cde-root/home
    (
        cd blob/cde-package
        mv -- * ..
    )
    rm -rf blob/cde-package
}

# Build the real mkblobexec.o / mkblobexec_dae.o that mkblob links against
# at runtime when producing a finished executable (not traced -- these are
# ordinary object files shipped as-is inside blob.tar.gz).
step_build_mkblobexec_objects() {
    echo "== Building mkblobexec.o / mkblobexec_dae.o =="
    local dae_src
    # Created in the current directory (not /tmp) so the compiler's
    # relative #include "sha256.h" still resolves next to it.
    dae_src="$(mktemp -p . mkblobexec_dae.XXXXXX.c)"
    cp mkblobexec.c "$dae_src"
    sed -i 's/deleteAtExit[[:space:]]*=[[:space:]]*0/deleteAtExit = 1/' "$dae_src"
    gcc -c "$dae_src" -o blob/mkblobexec_dae.o
    rm -f "$dae_src"
    gcc -c mkblobexec.c -o blob/mkblobexec.o
}

# Trim files from blob/ that shouldn't ship in the final blob.tar.gz.
step_cleanup_blob_dir() {
    echo "== Cleaning up blob/ =="
    rm -f blob/*.c blob/cde.uname blob/cde.log
    rm -rf blob/cde-root/usr/lib/locale/
}

# Package blob/ and build the final, statically linked mkblob binary.
step_finalize() {
    echo "== Packaging blob.tar.gz and building mkblob =="
    tar -zcf blob.tar.gz blob/
    gcc -DMKBLOB_VERSION="\"$VERSION\"" mkblob.c blobgz.S -o mkblob -static
    strip mkblob
    echo "ldd mkblob:"
    ldd mkblob || true
}

# Build the standalone mkblobid helper (unrelated to the mkblob/mkblobexec
# pipeline itself -- it's handed to a customer to report their MAC/UUID
# back, see readme.txt). Static, so it runs on any x86_64 Linux machine
# without needing a matching glibc, same as mkblob itself.
step_build_mkblobid() {
    echo "== Building static mkblobid =="
    gcc -Wall -Wextra -O2 -o mkblobid mkblobid.c -static
    strip mkblobid
    echo "ldd mkblobid:"
    ldd mkblobid || true
}

main() {
    step_clean
    step_unpack_cde
    step_build_cde_static
    step_build_okapi
    step_stage_blob_files
    step_bootstrap_cde_package
    step_flatten_cde_package
    step_build_mkblobexec_objects
    step_cleanup_blob_dir
    step_finalize
    step_build_mkblobid
    echo "Done."
}

main "$@"
