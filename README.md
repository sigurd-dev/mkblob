<div align="center">

# mkblob

**Turn any Linux program into one self-contained binary that runs anywhere — no rebuild, no dependency hunting, no install step on the target machine.**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-x86__64%20Linux-lightgrey.svg)](#requirements)
[![Latest release](https://img.shields.io/github/v/release/sigurd-dev/mkblob?sort=semver)](https://github.com/sigurd-dev/mkblob/releases)

</div>

---

`mkblob` traces which files a program actually touches — via a bundled, modified [CDE](https://github.com/pgbovine/CDE) tracer — and bakes every one of them, plus the program itself, straight into a single statically linked output binary. Copy that one file to any other x86_64 Linux box and it just runs, carrying its own copy of everything it needs.

Originally built to run OpenCV programs without rebuilding or reinstalling them on every machine — but it works on **any** Linux executable or script.

```sh
mkblob /bin/ls -o ls.blob -static   # bundle
./ls.blob                            # run it anywhere
```

## How it works

1. **Trace** — `mkblob` runs your program once under the CDE tracer and records every file, shared library and helper it opens.
2. **Gather** — all of those files are collected into one package alongside the program.
3. **Encrypt** *(optional)* — if you pass any of `-p` / `-mac` / `-expire` / `-uuid`, the package is encrypted with a key derived from those conditions (not just gated behind a check that could be patched away).
4. **Bake** — the package is embedded directly into a freshly compiled, statically linked binary using a self-contained toolchain, so the result depends on nothing from the build host.
5. **Run** — on the target machine the binary unpacks itself to a temp dir, runs the real program via `cde-exec`, and cleans up afterwards.

## Features

- **Portable by construction** — one static binary, zero runtime dependency chasing on the target.
- **Works on scripts too** — shell, Perl, Python, anything `execve`-able.
- **Bundle extra files/dirs** — pull in data files or whole directories the program needs at runtime (`-f`, `-d`).
- **Optional machine-locking & protection** — password, MAC, machine-UUID and/or expiry, cryptographically combinable (see the honest strength breakdown below).
- **`mkblobid` helper** — a tiny standalone tool you hand to a customer so they can report back their machine's MAC/UUID for a locked build.
- **`.rpm` / `.deb` packaging** included — the `.deb` is built natively with `dpkg-deb`, no `alien` required.

## Install

Grab the latest package or standalone binary from the [**Releases**](https://github.com/sigurd-dev/mkblob/releases) page:

```sh
# Any RPM-based distro (Fedora, RHEL, openSUSE, …)
sudo dnf install ./mkblob-2.1-1.x86_64.rpm

# Debian / Ubuntu family
sudo dpkg -i ./mkblob_2.1-1_amd64.deb

# Or just download the standalone binary and run it
chmod +x mkblob && ./mkblob -version
```

The packages are architecture-specific (x86_64) but **not tied to a distribution** — they contain only prebuilt, statically linked binaries with no dependencies, so the one `.rpm` installs on any RPM-based distro. Each release also ships `mkblobid`, the MAC/UUID reporter used with `-mac`/`-uuid`.

## Quick start

```sh
# Bundle any executable or script into one portable binary
mkblob /path/to/program -o program.blob -static

# Run it on any x86_64 Linux machine — no install needed
./program.blob
```

Need arguments passed to the wrapped program every time? Wrap it in a small shell script and run `mkblob` on that script instead.

Include extra files or a whole data directory:

```sh
mkblob /home/you/myapp -o myapp.blob -f /home/you/config.ini -d /home/you/assets -static
```

> **Use absolute paths** for the target executable and for `-f`/`-d` — `mkblob` switches to an internal temporary working directory while building, so relative source paths won't resolve. The `-o` output name may be relative (the blob is written to your current directory).

## Usage

```
mkblob <full path to executable> -o <output.blob> [flags...]
mkblob -version | --version | -v
```

| Flag | Description |
|------|-------------|
| `-o <file>` | Name of the finished blob **(required)**. |
| `-f <file>` | Include an extra file in the package. Repeatable. |
| `-d <dir>` | Include an entire directory, recursively. Repeatable. |
| `-static` | Produce a statically linked output (for a binary-compatible architecture). |
| `-dae` | *Delete at exit* — remove the unpacked temp files as soon as the program finishes, instead of waiting for the dynamic linker. Use it if your program loads files at runtime. |
| `-p` | Require a password to run the finished blob (prompted, hidden, min. 4 chars, 3 attempts). |
| `-mac <addr>` | Lock the blob to a machine with this network MAC (`aa:bb:cc:dd:ee:ff`). One address per build. |
| `-uuid <value>` | Lock the blob to a machine UUID — hardware `product_uuid` (root) or OS `machine-id` (anyone). Hyphens optional. Combinable with `-mac`. |
| `-expire <date>` | Refuse to run after this date (`YYYY-MM-DD`, through end of day UTC). |

`mkblobid` reports the exact `-mac`/`-uuid` values for a given machine, ready to paste into the command.

## Protection — and how strong each part really is

Passing any of `-p` / `-mac` / `-expire` / `-uuid` **encrypts** the program archive with a key derived (via 200,000 rounds of salted SHA-256, see `sha256.h`) from your password, and mixed further with the MAC, UUID and expiry you set. Without everything required, the key can't be reconstructed and the archive is useless bytes — there's no check to patch around. The output is always stripped, and a few security-relevant strings are obfuscated against a casual `strings`/disassembly look.

Be honest about what these buy you — they are **barriers against casual/accidental misuse, not unbreakable DRM**, and they are not equally strong:

| Flag | Binding | Real-world weakness |
|------|---------|---------------------|
| `-mac` | Cryptographically bound to the MAC | A MAC isn't secret and root can change it (`ip link set address`). |
| `-uuid` | Cryptographically bound to a machine UUID | Hardware `product_uuid` is hard to fake on physical machines; on a VM/VPS whoever controls the config sets it freely, and `machine-id` is as easy to change as a MAC. |
| `-expire` | Compared to the system clock at startup | The clock can be turned back and the check patched out — a universal limit of any offline expiry with no license server. |
| `-p` | Key derived from the password | Solid against casual use and static inspection; strength against brute force depends entirely on choosing a long passphrase. Not proof against a determined reverser with a debugger. |

This is **local, offline** protection with no server to phone home to — it raises the bar, it is not a guarantee. See [readme.txt](readme.txt) for the full threat model and why an external packer (e.g. Kiteshield) can't be layered on (it would fight `cde-exec` for `ptrace`).

## Building from source

Needs `glibc-static` (static linking is used throughout):

```sh
sudo dnf install glibc-static        # Fedora/RHEL family

./makeall.sh --unpack                # first build (unpacks CDE/); builds mkblob + mkblobid
./makeall.sh                         # subsequent rebuilds
```

`makeall.sh` builds a static `cde`/`cde-exec` and `okapi` from the sources in `CDE/`, stages them into `blob/`, packages that as `blob.tar.gz`, and compiles `mkblob` with the archive baked in. The version reported by `mkblob -version` comes from the [`VERSION`](VERSION) file.

Useful flags: `--clean` (remove previous artifacts first), `--unpack` (extract `CDE.tar.gz`, needed on a fresh checkout), `-h`.

### Packaging (.rpm / .deb)

```sh
./makeall.sh                 # build the binaries first
./build-packages.sh --all    # writes .rpm and .deb into ./dist/
```

`.rpm` is built via `rpmbuild` from [`mkblob.spec`](mkblob.spec); `.deb` is built directly with `dpkg-deb` (no `alien`, no `debhelper`). Both package only the already-built binaries plus docs — packaging never re-runs the compile. Version comes from `VERSION` for all three of the binary, the rpm and the deb.

## Requirements

Built and tested for **x86_64 Linux**. `makeall.sh` targets a Fedora/RHEL-family environment (fixed paths to gcc's internal `cc1`/`collect2`) and will likely need tweaking on other distributions or much newer toolchains — see *Known limitations* in [readme.txt](readme.txt).

## Documentation

- 📖 [readme.txt](readme.txt) — full manual: every flag, the complete protection threat model, a file-by-file breakdown, and known limitations.
- 🇳🇴 [lesmeg.txt](lesmeg.txt) — the Norwegian original (source of truth).

## See also

- Raspberry Pi version — <https://github.com/sigurd-dev/mkblob-rpi>
- Project page — <http://www.dagestad.info/mkblob>

## License

[GPLv3](LICENSE) · © Sigurd Dagestad, 2020–2026 · sigurd@dagestad.info
