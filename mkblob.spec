# Copyright (C) 2020-2026 Sigurd Dagestad <sigurd@dagestad.info>
# Licensed under the GNU General Public License v3.0 (see LICENSE).
#
# Version comes from build-packages.sh via --define "version ..." (which reads
# the VERSION file); the default below lets `rpmbuild mkblob.spec` still work
# built directly. Release is a plain integer and carries NO %{?dist} tag on
# purpose: mkblob ships prebuilt, statically linked binaries with no
# dependencies (AutoReqProv: no), so the package isn't tied to a distro
# release -- one mkblob-<ver>-1.x86_64.rpm installs on any RPM-based distro.
%{!?version: %global version 2.1}
Name:           mkblob
Version:        %{version}
Release:        1
Summary:        Bundle an executable with every library it needs into one portable binary

License:        GPL-3.0-only
URL:            https://github.com/sigurd-dev/mkblob

# mkblob and mkblobid are pre-built, statically linked binaries (see
# makeall.sh / readme.txt) -- this spec packages the results, it does
# not compile from source. Run ./makeall.sh first, then build this
# spec with _sourcedir pointing at the repo root (see build-packages.sh).
BuildArch:      x86_64
AutoReqProv:    no

%description
mkblob bundles an executable together with every shared library and
extra file it needs, producing a single self-contained binary that runs
on other Linux distributions without rebuilding or reinstalling
dependencies. Originally built to run OpenCV programs without rebuilding
on every target machine. Can optionally lock the produced binary to a
password, a specific machine (MAC address and/or hardware/OS UUID), and/
or an expiry date.

Includes mkblobid, a small helper that reports a machine's MAC
address(es) and UUID for use with mkblob's -mac/-uuid flags.

%prep
# Nothing to unpack -- mkblob/mkblobid are already built, see %%install.

%build
# Nothing to compile here -- run ./makeall.sh before building this
# package; see readme.txt.

%install
rm -rf %{buildroot}
install -Dm755 %{_sourcedir}/mkblob     %{buildroot}%{_bindir}/mkblob
install -Dm755 %{_sourcedir}/mkblobid   %{buildroot}%{_bindir}/mkblobid
install -Dm644 %{_sourcedir}/readme.txt %{buildroot}%{_docdir}/%{name}/readme.txt
install -Dm644 %{_sourcedir}/README.md  %{buildroot}%{_docdir}/%{name}/README.md
install -Dm644 %{_sourcedir}/LICENSE    %{buildroot}%{_licensedir}/%{name}/LICENSE

%files
%{_bindir}/mkblob
%{_bindir}/mkblobid
%doc %{_docdir}/%{name}/readme.txt
%doc %{_docdir}/%{name}/README.md
%license %{_licensedir}/%{name}/LICENSE

%changelog
* Sun Jul 05 2026 Sigurd Dagestad <sigurd@dagestad.info> - 2.1-1
- Add -p/-mac/-expire/-uuid protection for produced blobs (password,
  MAC-lock, expiry date, hardware/OS UUID lock)
- Add mkblobid helper to report a customer machine's MAC/UUID
- Harden mkblob.c/mkblobexec.c against real bugs (buffer overflow,
  uninitialized reads), safer temp handling, quoted shell arguments
- Fix noisy raw syscall-argument dumps from the bundled CDE/strace-4.6
  tracer on modern kernels

* Sat Aug 21 2021 Sigurd Dagestad <sigurd@dagestad.info> - 1.06-3
- Previous release (see git history)
