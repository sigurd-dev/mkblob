/*
 * mkblob - bundle an executable together with every shared library and
 * extra file it needs, producing a single self-contained binary that can
 * run, unmodified, on other Linux systems.
 *
 * (C) Sigurd Dagestad, Feb. 2020 - Jul. 2026
 * sigurd@dagestad.info
 *
 * Change log:
 *   Feb 2020: First release.
 *   Feb 2021: Fixed okapi path that was wrong.
 *   Jun 2021: Fixed PATH variable bug.
 *
 * Build notes:
 *   - To change the dynamic linker used by a produced binary:
 *     -Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
 *     https://stackoverflow.com/questions/14709202/change-ld-linux-location
 *   - Non-default shared library search path: -rpath, see
 *     https://stackoverflow.com/questions/8835108
 *   - strace-4.6/Makefile needs "-static" added to CFLAGS to produce a
 *     static cde/cde-exec; run `make` again inside the CDE dir afterwards.
 *   - Static linking requires glibc-static (dnf/yum install glibc-static).
 *     Mind the glibc static-linking licensing caveats:
 *     https://lwn.net/Articles/117972/
 *   - Background reading:
 *     https://developers.redhat.com/blog/2019/07/05/how-to-store-large-amounts-of-data-in-a-program/
 *     http://dcjtech.info/topic/embedding-files-in-c-source-code/
 */

#define _GNU_SOURCE /* strcasestr() */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/random.h>
#include <dirent.h>
#include <termios.h>
#include <time.h>

#include "sha256.h"

/* Version string. Set at build time via -DMKBLOB_VERSION="\"$(cat VERSION)\""
 * (see makeall.sh); falls back to "dev" for a plain `gcc mkblob.c` build. */
#ifndef MKBLOB_VERSION
#define MKBLOB_VERSION "dev"
#endif

/* Size and content of the embedded tar.gz blob, provided by blobgz.S */
extern const unsigned int size_tar;
extern const char blob_tar[];

#define PATHBUF (PATH_MAX + 256)

static const char *progname = "mkblob";

/* Marker prepended to an encrypted blob.tar.gz payload (see
 * encrypt_payload() below); mkblobexec.c checks for this to tell an
 * encrypted payload apart from a plain, unprotected one. */
#define ENC_MAGIC "MBE1"
#define ENC_ITERATIONS 200000u

struct options {
    char outfile[PATH_MAX];
    int make_static;
    int delete_at_exit;
    int protect;
    unsigned char enc_salt[16];
    unsigned char enc_key[32];
    int has_mac;
    unsigned char mac[6];
    int has_expire;
    int64_t expire_ts;
    int has_uuid;
    unsigned char uuid[16];
};

/* Any of -p/-mac/-expire/-uuid turns on payload encryption. */
static int wants_protection(const struct options *opts)
{
    return opts->protect || opts->has_mac || opts->has_expire || opts->has_uuid;
}

/* ---- small helpers ------------------------------------------------- */

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

#define SNPRINTF_CHECK(buf, ...)                                             \
    do {                                                                    \
        int _n = snprintf((buf), sizeof(buf), __VA_ARGS__);                 \
        if (_n < 0 || (size_t)_n >= sizeof(buf))                            \
            die("internal error: buffer too small (%s:%d)", __FILE__,       \
                __LINE__);                                                  \
    } while (0)

/* Append src to dst, dying instead of silently truncating/overflowing. */
static void xstrlcat(char *dst, size_t dstsz, const char *src)
{
    size_t dstlen = strlen(dst);
    if (dstlen >= dstsz)
        die("internal error: command buffer already full");
    size_t space = dstsz - dstlen - 1;
    size_t srclen = strlen(src);
    if (srclen > space)
        die("internal error: command buffer too small for argument list");
    memcpy(dst + dstlen, src, srclen + 1);
}

/* Run a command, treating a failure to even spawn a shell as fatal but
 * otherwise ignoring the command's own exit status (used for best-effort
 * housekeeping steps like chmod/rm). */
static void run_cmd(const char *cmd)
{
    if (system(cmd) == -1)
        die("failed to run command: %s", cmd);
}

/* Run a command and treat a non-zero exit status as fatal too. Used for
 * steps the rest of the build depends on. */
static void run_cmd_or_die(const char *cmd, const char *what)
{
    int status = system(cmd);
    if (status == -1)
        die("failed to run command for %s: %s", what, cmd);
    if (WIFSIGNALED(status))
        die("%s was killed by signal %d: %s", what, WTERMSIG(status), cmd);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        die("%s failed (exit code %d): %s", what, WEXITSTATUS(status), cmd);
}

/* Quote a string for safe use as a single shell word. Caller frees. */
static char *shell_quote(const char *s)
{
    size_t len = strlen(s);
    char *out = malloc(len * 4 + 3);
    if (!out)
        die("out of memory");
    char *p = out;
    *p++ = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            *p++ = '\'';
            *p++ = '\\';
            *p++ = '\'';
            *p++ = '\'';
        } else {
            *p++ = s[i];
        }
    }
    *p++ = '\'';
    *p = '\0';
    return out;
}

/* Print prompt, read a line with terminal echo disabled (falls back to a
 * plain read if stdin isn't a tty, e.g. piped input), strip the newline. */
static void read_hidden_line(const char *prompt, char *buf, size_t bufsz)
{
    struct termios oldt, newt;
    printf("%s", prompt);
    fflush(stdout);

    int have_tty = (tcgetattr(STDIN_FILENO, &oldt) == 0);
    if (have_tty) {
        newt = oldt;
        newt.c_lflag &= (tcflag_t)~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &newt);
    }

    if (!fgets(buf, (int)bufsz, stdin))
        buf[0] = '\0';

    if (have_tty)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
    printf("\n");

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';
}

static void read_password_twice(char *out, size_t outsz)
{
    char confirm[256];

    read_hidden_line("Password for this blob: ", out, outsz);
    if (strlen(out) < 4)
        die("password must be at least 4 characters");

    read_hidden_line("Confirm password: ", confirm, sizeof confirm);
    if (strcmp(out, confirm) != 0)
        die("passwords do not match");

    explicit_bzero(confirm, sizeof confirm);
}

/* Parses "aa:bb:cc:dd:ee:ff" into 6 raw bytes. Returns 0 on bad format. */
static int parse_mac(const char *s, unsigned char mac[6])
{
    if (strlen(s) != 17)
        return 0;
    unsigned int b[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3],
               &b[4], &b[5]) != 6)
        return 0;
    for (int i = 0; i < 6; i++)
        mac[i] = (unsigned char)b[i];
    return 1;
}

/* Parses a UUID (32 hex digits, hyphens optional/ignored, as used by both
 * /sys/class/dmi/id/product_uuid and /etc/machine-id) into 16 raw bytes.
 * Returns 0 on bad format. */
static int parse_uuid(const char *s, unsigned char out[16])
{
    char hex[33];
    int n = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '-')
            continue;
        if (n >= 32)
            return 0;
        hex[n++] = *p;
    }
    if (n != 32)
        return 0;
    hex[n] = '\0';
    for (int i = 0; i < 16; i++) {
        unsigned int byte;
        char pair[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        if (sscanf(pair, "%2x", &byte) != 1)
            return 0;
        out[i] = (unsigned char)byte;
    }
    return 1;
}

/* Parses "YYYY-MM-DD" into a UTC deadline timestamp: start of the day
 * *after* the given date, so the given date is usable through its end. */
static int64_t parse_expire_date(const char *s)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (!strptime(s, "%Y-%m-%d", &tm))
        die("invalid -expire date '%s', expected YYYY-MM-DD", s);
    tm.tm_mday += 1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    time_t t = timegm(&tm);
    if (t == (time_t)-1)
        die("could not compute an expiry timestamp for '%s'", s);
    return (int64_t)t;
}

/* Prompt for a password up front if -p was given (so bad input fails fast
 * before the expensive cde trace runs) -- empty string otherwise, which
 * is fine since -mac/-expire alone don't need a secret, just a base key
 * to fold the MAC/expiry constraints into via mix_key(). Generates a
 * random salt and derives the base key right away; only the derived key
 * lives in opts from here on, the plaintext password is wiped
 * immediately. */
static void prepare_protection(struct options *opts)
{
    char password[256] = "";
    if (opts->protect)
        read_password_twice(password, sizeof password);

    if (getrandom(opts->enc_salt, sizeof opts->enc_salt, 0) !=
        (ssize_t)sizeof opts->enc_salt)
        die("could not generate a random salt: %s", strerror(errno));

    derive_key(password, opts->enc_salt, ENC_ITERATIONS, opts->enc_key);
    explicit_bzero(password, sizeof password);
}

/* Encrypts workdir/blob.tar.gz in place with a key derived from the
 * password/MAC/expiry constraints, prefixed with a small header
 * mkblobexec.c needs to decrypt it again. Must run after
 * finalize_cde_package() (which produces that file) and before
 * compile_output() (which embeds whatever is at that path via
 * blobgz.S's .incbin). Unprotected builds never call this, so
 * blob.tar.gz stays exactly as before for them. */
static void encrypt_payload(const char *workdir, struct options *opts)
{
    char path[PATHBUF];
    SNPRINTF_CHECK(path, "%s/blob.tar.gz", workdir);

    FILE *fp = fopen(path, "rb");
    if (!fp)
        die("could not open %s: %s", path, strerror(errno));
    if (fseek(fp, 0, SEEK_END) != 0)
        die("could not seek in %s", path);
    long len = ftell(fp);
    if (len < 0 || fseek(fp, 0, SEEK_SET) != 0)
        die("could not determine size of %s", path);

    unsigned char *buf = malloc((size_t)len);
    if (!buf)
        die("out of memory");
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len)
        die("failed to read %s", path);
    fclose(fp);

    unsigned char expire_bytes[8] = {0};
    if (opts->has_expire) {
        uint64_t v = (uint64_t)opts->expire_ts;
        memcpy(expire_bytes, &v, sizeof v);
    }
    unsigned char mac_bytes[6] = {0};
    if (opts->has_mac)
        memcpy(mac_bytes, opts->mac, sizeof mac_bytes);
    unsigned char uuid_bytes[16] = {0};
    if (opts->has_uuid)
        memcpy(uuid_bytes, opts->uuid, sizeof uuid_bytes);

    unsigned char final_key[32];
    mix_key(opts->enc_key, expire_bytes, mac_bytes, uuid_bytes, final_key);
    explicit_bzero(opts->enc_key, sizeof opts->enc_key);

    keystream_xor(buf, (size_t)len, final_key, opts->enc_salt);
    explicit_bzero(final_key, sizeof final_key);

    fp = fopen(path, "wb");
    if (!fp)
        die("could not rewrite %s: %s", path, strerror(errno));
    uint32_t iterations = ENC_ITERATIONS;
    unsigned char requires_password = opts->protect ? 1 : 0;
    if (fwrite(ENC_MAGIC, 4, 1, fp) != 1 ||
        fwrite(&iterations, sizeof iterations, 1, fp) != 1 ||
        fwrite(opts->enc_salt, sizeof opts->enc_salt, 1, fp) != 1 ||
        fwrite(expire_bytes, sizeof expire_bytes, 1, fp) != 1 ||
        fwrite(&requires_password, sizeof requires_password, 1, fp) != 1 ||
        fwrite(buf, 1, (size_t)len, fp) != (size_t)len)
        die("failed to write encrypted %s", path);
    fclose(fp);

    free(buf);
}

static int looks_like_executable(const char *path)
{
    char *qpath = shell_quote(path);
    char cmd[PATHBUF];
    SNPRINTF_CHECK(cmd, "file -L %s 2>/dev/null", qpath);
    free(qpath);

    FILE *pipe = popen(cmd, "r");
    if (!pipe)
        return 0;
    char buf[2048] = "";
    if (!fgets(buf, sizeof buf, pipe))
        buf[0] = '\0';
    pclose(pipe);
    return strcasestr(buf, "shared object") != NULL ||
           strcasestr(buf, "executable") != NULL;
}

/* Copy one file or directory entry into the CDE package tree via okapi. */
static void copy_into_package(const char *workdir, const char *path)
{
    char *qpath = shell_quote(path);
    char cmd[PATHBUF];
    SNPRINTF_CHECK(cmd, "%s/okapi %s \"\" ./cde-package/cde-root/", workdir,
                   qpath);
    free(qpath);
    run_cmd(cmd);
}

static void copy_dir_recursive(const char *workdir, const char *dir_name)
{
    DIR *d = opendir(dir_name);
    if (!d)
        die("cannot open directory '%s': %s", dir_name, strerror(errno));

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *d_name = entry->d_name;
        if (strcmp(d_name, ".") == 0 || strcmp(d_name, "..") == 0)
            continue;

        char path[PATH_MAX];
        if (snprintf(path, sizeof path, "%s/%s", dir_name, d_name) >=
            (int)sizeof path)
            die("path too long: %s/%s", dir_name, d_name);

        copy_into_package(workdir, path);

        if (entry->d_type & DT_DIR)
            copy_dir_recursive(workdir, path);
    }

    if (closedir(d))
        die("could not close '%s': %s", dir_name, strerror(errno));
}

static void usage(const char *prog)
{
    fprintf(stderr, "mkblob %s\n\n", MKBLOB_VERSION);
    fprintf(stderr,
            "Usage: %s <full path to executable file> -o <output blob> "
            "[-f file] [-d dir] [-static] [-dae] [-p] [-mac addr] "
            "[-expire date] [-uuid value]\n"
            "       %s -version | --version | -v\n\n",
            prog, prog);
    fprintf(stderr, "where <full path to executable file> is the "
                    "executable file you want to make library independent.\n");
    fprintf(stderr, "      -o output file.\n");
    fprintf(stderr, "      -f file to include, use multiple if necessary.\n");
    fprintf(stderr, "      -d dir to include, use multiple if necessary.\n");
    fprintf(stderr,
            "      -static, make a static executable for use on compatible "
            "arch.\n");
    fprintf(stderr,
            "      -dae, delete at end, delete files from "
            "/tmp/<randomdir> when finished, to be used if program needs "
            "to load files at runtime.\n");
    fprintf(stderr,
            "      -p, prompt for a password that must be entered to run "
            "the produced blob.\n");
    fprintf(stderr,
            "      -mac aa:bb:cc:dd:ee:ff, only run on a machine that has "
            "a network interface with this MAC address (one address, one "
            "machine per build).\n");
    fprintf(stderr,
            "      -expire YYYY-MM-DD, refuse to run after this date.\n");
    fprintf(stderr,
            "      -uuid value, only run on a machine with this hardware "
            "or OS-install UUID (from /sys/class/dmi/id/product_uuid, "
            "root only, or /etc/machine-id, readable by anyone). Can be "
            "combined with -mac. Hyphens optional.\n\n");
    fprintf(stderr,
            "-p/-mac/-expire/-uuid are deterrents against casual use, not "
            "unbreakable protection: anyone with the file can still "
            "extract or patch it. -mac/-uuid are cryptographically tied "
            "to the payload (no matching value, no key, no way around it "
            "short of spoofing that MAC/UUID); -expire is enforced via "
            "the system clock like any offline trial, and can't be more "
            "solid than that. See readme.txt for details.\n\n");
    fprintf(stderr, "Example: mkblob /usr/bin/ls -o ls.blob -static\n\n");
    fprintf(stderr,
            "Project was originally started to make opencv programs able "
            "to run without recompiling/rebuilding/installing.\n\n");
    fprintf(stderr, "2020-2026 (C) Sigurd Dagestad (sigurd@dagestad.info)\n\n");
}

/* ---- build steps ---------------------------------------------------- */

static void create_temp_workdir(char workdir[PATHBUF])
{
    if (snprintf(workdir, PATHBUF, "/tmp/mkblob.XXXXXX") >= PATHBUF)
        die("internal error: workdir buffer too small");
    if (!mkdtemp(workdir))
        die("could not create a temporary working directory: %s",
            strerror(errno));
}

static void extract_embedded_blob(const char *workdir)
{
    char blobtar[PATHBUF];
    SNPRINTF_CHECK(blobtar, "%s/blob_input.tar.gz", workdir);

    FILE *f = fopen(blobtar, "wb");
    if (!f)
        die("could not create %s: %s", blobtar, strerror(errno));
    if (fwrite(blob_tar, size_tar, 1, f) != 1)
        die("failed to write embedded package to %s", blobtar);
    fclose(f);

    char tarcmd[PATHBUF * 2];
    SNPRINTF_CHECK(tarcmd, "tar -zxf %s -C %s --strip-components=1", blobtar,
                   workdir);
    run_cmd_or_die(tarcmd, "extracting embedded CDE package");

    remove(blobtar);
}

/* Parses argv, copies any -f/-d payloads into the package, and builds the
 * "export PATH=...; cde <target> ..." command to be run afterwards.
 * Returns a malloc'd copy of the first positional (target) argument. */
static char *process_arguments(int argc, char *argv[], const char *workdir,
                                struct options *opts, char *command,
                                size_t command_sz)
{
    strcpy(opts->outfile, "a.out");
    opts->make_static = 0;
    opts->delete_at_exit = 0;
    opts->protect = 0;
    opts->has_mac = 0;
    opts->has_expire = 0;
    opts->expire_ts = 0;
    opts->has_uuid = 0;

    const char *path_env = getenv("PATH");
    char header[8192];
    SNPRINTF_CHECK(header, "export PATH=\"%s:%s\"; cde ", workdir,
                   path_env ? path_env : "");
    command[0] = '\0';
    xstrlcat(command, command_sz, header);

    char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc)
                die("-o requires an output filename");
            SNPRINTF_CHECK(opts->outfile, "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-dae") == 0) {
            opts->delete_at_exit = 1;
        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc)
                die("-d requires a directory argument");
            copy_dir_recursive(workdir, argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc)
                die("-f requires a file argument");
            copy_into_package(workdir, argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-static") == 0) {
            opts->make_static = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            opts->protect = 1;
        } else if (strcmp(argv[i], "-mac") == 0) {
            if (i + 1 >= argc)
                die("-mac requires a MAC address (aa:bb:cc:dd:ee:ff)");
            if (!parse_mac(argv[i + 1], opts->mac))
                die("invalid -mac address '%s', expected aa:bb:cc:dd:ee:ff",
                    argv[i + 1]);
            opts->has_mac = 1;
            i++;
        } else if (strcmp(argv[i], "-expire") == 0) {
            if (i + 1 >= argc)
                die("-expire requires a date (YYYY-MM-DD)");
            opts->expire_ts = parse_expire_date(argv[i + 1]);
            opts->has_expire = 1;
            i++;
        } else if (strcmp(argv[i], "-uuid") == 0) {
            if (i + 1 >= argc)
                die("-uuid requires a value (from /sys/class/dmi/id/"
                    "product_uuid or /etc/machine-id on the target "
                    "machine)");
            if (!parse_uuid(argv[i + 1], opts->uuid))
                die("invalid -uuid value '%s', expected a 32-hex-digit "
                    "UUID (hyphens optional)",
                    argv[i + 1]);
            opts->has_uuid = 1;
            i++;
        } else {
            if (!looks_like_executable(argv[i]))
                printf("%s does not seem to be an executable file.\nI "
                       "might be wrong, so I'll try anyway...\n",
                       argv[i]);

            if (!input)
                input = strdup(argv[i]);

            char *qarg = shell_quote(argv[i]);
            xstrlcat(command, command_sz, qarg);
            xstrlcat(command, command_sz, " ");
            free(qarg);
        }
    }

    if (!input)
        die("no target executable given, see usage");

    return input;
}

static void write_execute_wrapper(const char *workdir, const char *input)
{
    char execfile[PATHBUF];
    SNPRINTF_CHECK(execfile, "%s/execute.cde", workdir);

    char *qinput = shell_quote(input);

    FILE *fp = fopen(execfile, "w");
    if (!fp)
        die("could not create %s: %s", execfile, strerror(errno));
    fprintf(fp,
            "#!/bin/sh\n"
            "DN=\"$(dirname \"$(readlink -f \"${0}\")\")\"\n"
            "\"$DN/cde-exec\" %s \"$@\"\n",
            qinput);
    fclose(fp);
    free(qinput);
}

static void write_blob_asm(const char *workdir)
{
    char blobgzS[PATHBUF];
    SNPRINTF_CHECK(blobgzS, "%s/blobgz.S", workdir);

    FILE *fp = fopen(blobgzS, "w");
    if (!fp)
        die("could not create %s: %s", blobgzS, strerror(errno));
    fprintf(fp,
            "    .global blob_tar\n"
            "    .global size_tar\n"
            "    .section .rodata\n"
            "blob_tar:\n"
            "    .incbin \"%s/blob.tar.gz\"\n"
            "1:\n"
            "size_tar:\n"
            "    .int 1b - blob_tar\n",
            workdir);
    fclose(fp);
}

/* Move the lib/lib64 layout so both Red Hat- and Debian-style dynamic
 * linker search paths resolve, then re-package cde-package/ as blob.tar.gz. */
static void finalize_cde_package(const char *workdir)
{
    char cmd[PATHBUF];

    SNPRINTF_CHECK(cmd, "mv %s/execute.cde %s/cde-package/execute.cde",
                   workdir, workdir);
    run_cmd_or_die(cmd, "moving execute.cde into the package");

    SNPRINTF_CHECK(cmd, "rm -f %s/cde-package/cde.log", workdir);
    run_cmd(cmd);
    SNPRINTF_CHECK(cmd, "rm -f %s/cde-package/cde.uname", workdir);
    run_cmd(cmd);

    SNPRINTF_CHECK(cmd, "rm -rf %s/cde-package/cde-root/home", workdir);
    run_cmd(cmd);

    char gnudir[PATHBUF];
    SNPRINTF_CHECK(gnudir, "%s/cde-package/cde-root/usr/lib/x86_64-linux-gnu",
                   workdir);

    if (access(gnudir, F_OK) != 0) {
        /* Red Hat-style layout: add a Debian-style alias. */
        char libdir[PATHBUF];
        SNPRINTF_CHECK(libdir, "%s/cde-package/cde-root/usr/lib", workdir);
        if (chdir(libdir) != 0)
            die("could not chdir to %s: %s", libdir, strerror(errno));
        run_cmd("ln -s ../lib64 ./x86_64-linux-gnu");
    } else {
        /* Debian-style layout: add a Red Hat-style alias. */
        char usrdir[PATHBUF];
        SNPRINTF_CHECK(usrdir, "%s/cde-package/cde-root/usr", workdir);
        if (chdir(usrdir) != 0)
            die("could not chdir to %s: %s", usrdir, strerror(errno));
        run_cmd("cp ./lib64/* ./lib/x86_64-linux-gnu/");
        run_cmd("mv lib64 lib64.orig");
        run_cmd("ln -s ./lib/x86_64-linux-gnu ./lib64");
    }

    if (chdir(workdir) != 0)
        die("could not chdir to %s: %s", workdir, strerror(errno));

    SNPRINTF_CHECK(cmd, "tar -zcf %s/blob.tar.gz -C %s cde-package/", workdir,
                   workdir);
    run_cmd_or_die(cmd, "packaging blob.tar.gz");
}

static void compile_output(const char *workdir, const struct options *opts)
{
    char cmd[PATHBUF * 3];
    SNPRINTF_CHECK(cmd,
                   "%s/gcc.cde -L%s -I%s %s/%s %s/blobgz.S %s -o %s/%s -lpthread",
                   workdir, workdir, workdir, workdir,
                   opts->delete_at_exit ? "mkblobexec_dae.o" : "mkblobexec.o",
                   workdir, opts->make_static ? "-static" : "", workdir,
                   opts->outfile);
    run_cmd_or_die(cmd, "compiling the output binary");

    char stripcmd[PATHBUF];
    SNPRINTF_CHECK(stripcmd, "strip %s/%s", workdir, opts->outfile);
    run_cmd(stripcmd);
}

static void install_output(const char *workdir, const struct options *opts,
                            const char *startdir)
{
    char src[PATHBUF];
    char dst[PATHBUF];
    SNPRINTF_CHECK(src, "%s/%s", workdir, opts->outfile);
    SNPRINTF_CHECK(dst, "%s%s", startdir, opts->outfile);

    if (rename(src, dst) != 0) {
        /* rename() fails across filesystems (e.g. /tmp on tmpfs); fall
         * back to a copy+remove via the shell. */
        char cmd[PATHBUF * 2];
        SNPRINTF_CHECK(cmd, "mv %s %s", src, dst);
        run_cmd_or_die(cmd, "moving the output binary into place");
    }

    char rmcmd[PATHBUF];
    SNPRINTF_CHECK(rmcmd, "rm -rf %s", workdir);
    run_cmd(rmcmd);
}

int main(int argc, char *argv[])
{
    if (argc >= 2 &&
        (strcmp(argv[1], "-version") == 0 ||
         strcmp(argv[1], "--version") == 0 ||
         strcmp(argv[1], "-v") == 0)) {
        printf("mkblob %s\n", MKBLOB_VERSION);
        return EXIT_SUCCESS;
    }

    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    char startdir[PATH_MAX];
    if (!getcwd(startdir, sizeof startdir))
        die("could not determine the current directory: %s",
            strerror(errno));
    xstrlcat(startdir, sizeof startdir, "/");

    char workdir[PATHBUF];
    create_temp_workdir(workdir);
    extract_embedded_blob(workdir);

    if (chdir(workdir) != 0)
        die("could not chdir to %s: %s", workdir, strerror(errno));
    run_cmd("mkdir -p cde-package/cde-root/");
    run_cmd("chmod -R 755 cde-package/");

    struct options opts;
    char command[8192];
    char *input = process_arguments(argc, argv, workdir, &opts, command,
                                     sizeof command);

    if (wants_protection(&opts))
        prepare_protection(&opts);

    printf("Making portable executable blob, wait up...\n");

    char chmodcmd[PATHBUF];
    SNPRINTF_CHECK(chmodcmd, "chmod -R 755 %s", workdir);
    run_cmd(chmodcmd);

    write_execute_wrapper(workdir, input);
    write_blob_asm(workdir);
    free(input);

    run_cmd_or_die(command, "tracing the target executable with cde");

    finalize_cde_package(workdir);
    if (wants_protection(&opts))
        encrypt_payload(workdir, &opts);
    compile_output(workdir, &opts);
    install_output(workdir, &opts, startdir);

    printf("Done!\n");
    return EXIT_SUCCESS;
}
