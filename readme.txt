MKBLOB - README
================

(This is the English translation of lesmeg.txt, the Norwegian original.
Ship/packaging-wise this file is the canonical manual; lesmeg.txt stays
in the repo as the source-of-truth Norwegian version.)

What is this?
--------------
mkblob is a tool that bundles an executable together with every shared
library and other file it needs to run, so the result is a single,
self-contained file that can run on other Linux distributions without
rebuilding or installing anything. It uses CDE (Code, Data and
Environment) to trace which files a program actually touches, and
bundles those files into the finished "blob".

The project was originally built to run OpenCV programs without having
to rebuild/reinstall them on every single machine.

Example: mkblob /bin/ls -o ls.blob -static


Building
--------
Build with: ./makeall.sh

Made for x86_64 CentOS 8 / RHEL 8. You will likely need to adjust the
script to build on other distributions. Static builds require the
glibc-static package (dnf install glibc-static).

Flags to makeall.sh:
  --clean    Remove previous build artifacts (blob/, blob.tar.gz, cde)
             before building.
  --unpack   Unpack CDE.tar.gz before building. Needed on a completely
             fresh checkout; can be skipped on repeat builds where
             ./CDE already exists.
  -h/--help  Show help.

The script builds a static cde/cde-exec and okapi from the sources in
CDE/, stages a staging directory (blob/) with those tools plus
mkblobexec.o/mkblobexec_dae.o, packages it as blob.tar.gz, and finally
compiles the actual mkblob executable with that archive baked in.


Usage
-----
mkblob <full path to executable> -o <output-blob> [flags...]

Flags:
  -o <file>    Name of the finished blob (required).
  -f <file>    Include an extra file in the package. Can be used
               multiple times.
  -d <dir>     Include an entire directory recursively in the package.
               Can be used multiple times.
  -static      Produce a statically linked output file (for use on a
               binary-compatible architecture).
  -dae         "Delete at exit" - delete the files in
               /tmp/<random-dir> as soon as the program has finished,
               instead of waiting for the underlying dynamic linker to
               be done. Use this if your program itself loads files at
               runtime and you want cleanup to happen earlier.
  -p           Require a password to run the finished blob (see the
               dedicated section below).
  -mac <addr>  Lock the blob to one specific machine: it refuses to run
               unless the machine has a network interface with this MAC
               address (format aa:bb:cc:dd:ee:ff). One address per
               build - if you need several licensed machines, build one
               blob per machine.
  -expire <date>
               The blob refuses to run after this date (format
               YYYY-MM-DD, valid through the end of the given day,
               UTC).
  -uuid <value>
               Lock the blob to one specific machine via a machine UUID
               instead of (or in addition to) a MAC address. Get it
               from the target machine with either "cat
               /etc/machine-id" (doesn't need root) or "sudo cat
               /sys/class/dmi/id/product_uuid" (needs root, but is tied
               to the hardware, not the OS install). Hyphens in the
               value are optional.

               Easier way to get these values: build and hand
               mkblobid.c to the customer (see its own section below).

mkblob can be used on binaries as well as scripts (shell, perl, python,
etc). If your program needs command-line arguments you want to pass
every time, make a small wrapper script that does that and run mkblob
on the script instead.


Protection (-p, -mac, -expire, -uuid)
---------------------------------------
All four can be used individually or combined on the same build. Using
at least one of them encrypts the program archive (blob.tar.gz) -- not
just gates access behind a check you could patch away. The key is built
from:

  - your password if -p is used (an empty string otherwise), salted and
    run through 200,000 rounds of SHA-256 (see sha256.h, derive_key()),
  - mixed further with the expiry date if -expire is used,
  - the MAC address if -mac is used,
  - and the machine UUID if -uuid is used (see sha256.h, mix_key()).

Without everything required (correct password AND correct machine AND
within the date deadline), the correct key cannot be reconstructed, and
the encrypted archive is useless garbage -- there is no "shortcut" to
extract the original program without actually satisfying the
conditions.

-p: With -p, mkblob asks you to type in a password (hidden, no echo)
    when building the blob. The password must be entered twice and must
    be at least 4 characters. At runtime, you're asked for the password
    (hidden entry). A wrong password can be tried 3 times before the
    blob refuses to run any further and cleans up after itself.

-mac: Checked automatically at runtime (no extra input) -- the blob
    tries this machine's real network interface addresses (from
    /sys/class/net/*/address) against what was baked in at build time.
    If the MAC restriction is active and no local address matches, it
    simply doesn't run.

-expire: Checked BEFORE anything else, right at startup, against the
    system clock. If the deadline has passed, the blob exits
    immediately with a clear message -- without asking for a password
    first.

-uuid: Checked automatically at runtime, just like -mac, and can be
    combined with -mac (then both must match). The blob first tries the
    machine's /sys/class/dmi/id/product_uuid (hardware UUID); if it's
    not readable (common for a non-root user -- the file is root-only
    on most systems), it automatically falls back to /etc/machine-id
    (OS-install UUID, readable by everyone) without crashing or
    requiring root. This is deliberately made robust across all kinds
    of systems/permissions, specifically so the blob works as a
    universal tool no matter who runs it -- see "Known limitations" for
    more detail on which environments this has been tested against.

The output file is always automatically stripped of symbols, and a
handful of security-relevant text strings are obfuscated in the
compiled file, to make a quick look with "strings" or a disassembly
tool (e.g. IDA) less useful.

IMPORTANT -- be honest with yourself about what these actually protect
against, and note that they are NOT equally strong:

  -mac and -uuid are both genuinely cryptographically bound: without
  the right value it is physically impossible to compute the right
  key, there is no "if" check to patch away. The weakness lies
  elsewhere for each of them:

    * A MAC address isn't secret (visible to everyone on the same
      network) and can be changed by anyone with root access
      (ip link set address ...).
    * /sys/class/dmi/id/product_uuid is burned into the hardware and
      thus genuinely harder to fake on PHYSICAL machines -- but on a
      virtual machine (cloud server/VPS), whoever controls the VM
      configuration sets this value freely, and cloned VM images can
      even end up sharing the same UUID by mistake. /etc/machine-id
      (the fallback) is about as easy to change as a MAC address.

  So neither of them stops an attacker who knows the target value and
  is willing to change their own machine/VM to match it -- they stop
  accidental/casual use on the wrong machine.

  -expire is fundamentally different: "now" changes on every run, so it
  can only ever be enforced by comparing against the system clock right
  then and there. That check can be patched out, and the clock itself
  can be turned back. This is a universal limitation of any offline
  expiry scheme -- nothing specific to this implementation -- and
  cannot be made stronger without an external license server to check
  against, which mkblob deliberately does not have.

  -p (password) is a solid barrier against casual use, unauthorized
  people poking at the file, and quick static inspection. It is NOT
  real, unbreakable copy protection (DRM). A technically competent
  person with enough time, a debugger, and the will to analyze the
  binary can in principle still get past it. Its strength against raw
  password guessing (brute force) depends heavily on HOW GOOD the
  password you choose is -- use a proper long passphrase, not a short
  or guessable word.

  In general: this is local, offline protection with no external server
  to check against. All of this raises the bar, it is not a guarantee.

  We also considered external "packers" (like Kiteshield) for another
  layer of protection, but rejected it: such tools use ptrace()
  themselves to protect themselves, which collides directly with
  cde-exec (which runs the wrapped program) already using ptrace() on
  that exact same process on every run. Linux only allows one "tracer"
  per process at a time, so the combination would likely just break
  execution instead of protecting anything.


Getting MAC/UUID from a customer's machine (mkblobid.c)
---------------------------------------------------------
mkblobid.c is a small, standalone program (not part of the
mkblob/mkblobexec pipeline) that prints exactly the values -mac and
-uuid expect, ready to copy into your mkblob command:

  gcc -O2 -o mkblobid mkblobid.c -static

Hand the finished (ideally statically linked) mkblobid file to the
customer, have them run it, and have them send the result back to you.
It:
  - lists every network interface's MAC address (skips lo),
  - shows the hardware UUID (/sys/class/dmi/id/product_uuid) if run as
    root, otherwise a clear message that it requires root,
  - always shows the OS-install UUID (/etc/machine-id, doesn't require
    root),
  - and prints a ready-made example of the actual mkblob command with
    these values filled in.


File overview
-------------
mkblob.c
    The build tool itself. Takes your arguments, traces the given
    program with cde, gathers the files into a temporary directory,
    encrypts the payload if -p/-mac/-expire/-uuid is used, and compiles
    the finished, self-contained executable.

mkblobexec.c
    Compiled into EVERY blob mkblob produces. At runtime it unpacks the
    embedded archive to /tmp, asks for a password if the blob is
    protected, runs the actual program via cde-exec, and cleans up
    afterwards (in its own background thread, since the wrapped program
    might go on to run in the background/daemonize itself).

sha256.h
    Self-contained (header-only) implementation of SHA-256, plus helper
    functions for salted/iterated key derivation (derive_key), mixing a
    MAC address/expiry date/machine UUID into the key (mix_key), and a
    simple stream cipher built on the same hash (keystream_xor) -- used
    by -p/-mac/-expire/-uuid. No external crypto libraries needed.

mkblobid.c
    Standalone helper tool for getting the MAC address(es) and UUID
    from a customer's machine, for use with -mac/-uuid. See its own
    section above.

blobgz.S
    Assembly file that, via ".incbin", embeds a tar.gz file directly
    into an executable as the symbols blob_tar/size_tar. Used both when
    mkblob itself is built (embedding the tools in blob/) and by mkblob
    itself when it generates the finished blob (embedding your program
    archive).

makeall.sh
    Builds cde/cde-exec (from CDE/strace-4.6) and okapi statically from
    source, assembles the staging directory in blob/, and finally
    compiles mkblob itself. See "Building" above for flags.

makeclean.sh
    Removes all build artifacts (CDE/, blob/, blob.tar.gz, cde, mkblob,
    mkblobid, okapi, dist/) for a completely clean directory.

mkblob.spec
    RPM spec. Packages the already-built mkblob/mkblobid files (does
    not build from source itself) -- see "Packaging" above.

build-packages.sh
    Builds .rpm (via rpmbuild) and/or .deb (directly with dpkg-deb, no
    alien) from the already-compiled mkblob/mkblobid files. See
    "Packaging" above.

CDE/
    Source code for CDE (Code, Data and Environment), a modified
    strace-4.6, which mkblob uses to trace and capture the file
    dependencies of the program being packaged. This code has been
    adapted specifically for this project -- among other things, an old
    bug is fixed where unknown/unhandled syscalls on modern kernels
    could leak raw information into the program's own output instead of
    being silently ignored (see CDE/strace-4.6/syscall.c).

templates/
    Base files copied into every cde package during the build (gcc.cde,
    cc1, as, ld, collect2, cde.options, among others) -- a "fake"
    toolchain mkblob uses to compile the finished blob completely
    independently of the system it's running on.


Packaging (.rpm / .deb)
-------------------------
1. Build first: ./makeall.sh (produces mkblob and mkblobid).
2. Package: ./build-packages.sh [--rpm] [--deb] [--all]  (--all is the
   default)

Writes mkblob-<version>.<arch>.rpm and mkblob_<version>_amd64.deb to
./dist/. Both packages only the two already-built binaries plus
readme.txt/README.md/LICENSE as documentation -- the actual
mkblob/mkblobexec build (makeall.sh) is NEVER run as part of packaging.

.rpm is built with rpmbuild from mkblob.spec (needs rpm-build).
.deb is built directly with dpkg-deb -- no alien, no debhelper/build-
essential needed, works fine on Fedora or Debian-based systems. If you
would rather have a full debian/ source-package setup (to upload to a
real Debian/Ubuntu repo), that isn't set up here for now, but is a
reasonable extension of build-packages.sh if you need it later.

Other package formats (Arch packages/PKGBUILD, Alpine/APK, Flatpak/
Snap/AppImage) are not set up. Since mkblob is already a single,
self-contained, statically linked file with no dependencies, universal
"bundle everything yourself" formats like Flatpak/Snap/AppImage add
little extra value here -- they're built for programs with many
runtime dependencies, not single binaries. rpm and deb cover the vast
majority of Linux users.


Known limitations
------------------
- Only tested/built for x86_64 Linux.
- Building assumes an environment fairly similar to CentOS 8/RHEL 8
  (among other things, fixed paths to gcc's internal tools
  cc1/collect2). Other distributions and newer gcc versions may need
  adjustments to makeall.sh.
- The protection flags (-p, -mac, -expire, -uuid) are barriers, not
  guarantees -- see the dedicated section above for how strong each of
  them actually is.
- -mac/-uuid support only one address/UUID per build, not a list of
  several allowed machines.
- -uuid falls back safely from product_uuid (root-only) to machine-id
  (readable by everyone) so it works no matter which user runs the
  blob -- but on systems with neither available (very minimal/old
  setups, or containers where neither /sys/class/dmi nor
  /etc/machine-id is available), -uuid has no local value to match
  against, and the blob will then always refuse to run there.
- cde/cde-exec actively uses ptrace() on every run of a wrapped blob.
  This is incompatible with other tools that themselves want exclusive
  ptrace access to the same process (e.g. your own debuggers, or
  packers like Kiteshield).


License
-------
GPLv3.

Sigurd Dagestad, 2020-2026 (sigurd@dagestad.info)
