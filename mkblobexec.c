/*
 * mkblobexec.c
 *
 * Self-extracting launcher compiled into every "blob" produced by mkblob.
 * On startup it unpacks the embedded CDE package to a temporary directory,
 * runs the wrapped program through cde-exec, and cleans the temporary
 * directory up again once the program (and any process it spawned or
 * daemonized into) has exited.
 *
 * (C) Sigurd Dagestad, Feb. 2020
 * sigurd@dagestad.info
 *
 * Change log:
 *   Jun 2021: Fixed PATH variable bug.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <termios.h>
#include <time.h>
#include <dirent.h>

#include "sha256.h"

/* Size and content of the embedded tar.gz blob, provided by blobgz.S. May
 * be a plain tar.gz (unprotected build) or, if it starts with ENC_MAGIC,
 * a payload encrypted by mkblob.c's encrypt_payload() -- see
 * extract_embedded_blob() below. */
extern const unsigned int size_tar;
extern const char blob_tar[];

/* magic(4) + iterations(4) + salt(16) + expire_ts(8) + requires_password(1) */
#define ENC_MAGIC "MBE1"
#define ENC_HEADER_LEN 33
#define MAX_PASSWORD_ATTEMPTS 3
#define MAX_LOCAL_MACS 16
#define MAX_LOCAL_UUIDS 2

/* Set to 1 at build time (via mkblobexec_dae.o) for programs that need
 * their unpacked files to remain in place after cde-exec returns, e.g.
 * because they daemonize and keep loading files at runtime. Do not edit
 * this by hand; makeall.sh generates the "_dae" variant with sed. */
int deleteAtExit = 0;

#define PATHBUF (PATH_MAX + 256)

/* Security-relevant strings are kept out of the binary's string table as
 * plain text (defeats a quick `strings`/IDA skim) and decoded into a
 * caller-supplied buffer on use. Not meant to stop a determined analyst
 * who actually reads the disassembly -- just the fast path. */
#define OBF_KEY 0x5a
static const unsigned char obf_password_prompt[] = {
    0x0a, 0x3b, 0x29, 0x29, 0x2d, 0x35, 0x28, 0x3e, 0x60, 0x7a,
};
static const unsigned char obf_incorrect_password[] = {
    0x13, 0x34, 0x39, 0x35, 0x28, 0x28, 0x3f, 0x39, 0x2e, 0x7a,
    0x2a, 0x3b, 0x29, 0x29, 0x2d, 0x35, 0x28, 0x3e, 0x74,
};
static const unsigned char obf_progname[] = {
    0x37, 0x31, 0x38, 0x36, 0x35, 0x38, 0x3f, 0x22, 0x3f, 0x39,
};
static const unsigned char obf_expired[] = {
    0x0e, 0x32, 0x33, 0x29, 0x7a, 0x38, 0x36, 0x35, 0x38, 0x7a, 0x32,
    0x3b, 0x29, 0x7a, 0x3f, 0x22, 0x2a, 0x33, 0x28, 0x3f, 0x3e, 0x74,
};
static const unsigned char obf_not_authorized[] = {
    0x0e, 0x32, 0x33, 0x29, 0x7a, 0x37, 0x3b, 0x39, 0x32, 0x33, 0x34,
    0x3f, 0x7a, 0x33, 0x29, 0x7a, 0x34, 0x35, 0x2e, 0x7a, 0x3b, 0x2f,
    0x2e, 0x32, 0x35, 0x28, 0x33, 0x20, 0x3f, 0x3e, 0x7a, 0x2e, 0x35,
    0x7a, 0x28, 0x2f, 0x34, 0x7a, 0x2e, 0x32, 0x33, 0x29, 0x7a, 0x2a,
    0x28, 0x35, 0x3d, 0x28, 0x3b, 0x37, 0x74,
};

/* out must be at least n+1 bytes. */
static void deobfuscate(char *out, const unsigned char *enc, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = (char)(enc[i] ^ OBF_KEY);
    out[n] = '\0';
}

static char progname[16];

/* Shared between main() and the cleanup-watcher thread. */
static char g_workdir[PATHBUF];
static char g_rm_cmd[PATHBUF];
static char g_pidof_cmd[PATHBUF];

/* Set by check_not_debugged(); mixed into the decryption key so that,
 * under a debugger, even the correct password quietly decrypts to
 * garbage instead of printing anything an analyst could search for. */
static unsigned char g_tamper = 0;

/* ---- small helpers --------------------------------------------------- */

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

static void run_cmd(const char *cmd)
{
    if (system(cmd) == -1)
        die("failed to run command: %s", cmd);
}

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

/* Print prompt, read a line with terminal echo disabled (falls back to a
 * plain read if stdin isn't a tty), strip the newline. */
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

/* Best-effort debugger detection: checks /proc/self/status's TracerPid.
 * Does not react visibly -- see g_tamper. This only concerns this
 * process; it has no bearing on CDE's own ptrace use against the wrapped
 * program later, which is a separate exec entirely.
 *
 * Deliberately NOT using a PTRACE_TRACEME self-trace here: tried it, and
 * it made the process come out (mis)tracked by whatever actually spawned
 * it in some environments (init systems, containers, IDEs, sandboxes --
 * anything that already has its own relationship with the process), with
 * the traced process left unable to run normally. A security check that
 * breaks the program outright for legitimate users is worse than a
 * weaker check that doesn't. */
static void check_not_debugged(void)
{
    int suspicious = 0;

    FILE *fp = fopen("/proc/self/status", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof line, fp)) {
            const char *p = strstr(line, "TracerPid:");
            if (p) {
                if (strtol(p + strlen("TracerPid:"), NULL, 10) != 0)
                    suspicious = 1;
                break;
            }
        }
        fclose(fp);
    }

    if (suspicious)
        g_tamper = 0x37;
}

/* ---- cleanup watcher --------------------------------------------------
 *
 * The wrapped program may background/daemonize itself before the
 * synchronous cde-exec call in main() returns. This thread watches for
 * the loaded dynamic linker's process to actually disappear and removes
 * the temporary directory once it does, unless the program was built
 * with -dae (in which case main() already deletes it up front and the
 * program is expected to have finished loading everything it needs by
 * the time cde-exec returns). */
static void *cleanup_watcher(void *unused)
{
    (void)unused;
    for (;;) {
        if (system(g_pidof_cmd) != 0) {
            /* The process is gone. */
            if (!deleteAtExit)
                run_cmd(g_rm_cmd);
            break;
        }
        usleep(100000); /* 100 ms */
    }
    return NULL;
}

/* ---- build steps ------------------------------------------------------- */

static void create_temp_workdir(char workdir[PATHBUF])
{
    if (snprintf(workdir, PATHBUF, "/tmp/mkblobexec.XXXXXX") >= PATHBUF)
        die("internal error: workdir buffer too small");
    if (!mkdtemp(workdir))
        die("could not create a temporary working directory: %s",
            strerror(errno));
}

static void write_and_extract(const char *workdir, const char *blobtar,
                               const unsigned char *data, size_t len)
{
    FILE *f = fopen(blobtar, "wb");
    if (!f)
        die("could not create %s: %s", blobtar, strerror(errno));
    if (fwrite(data, 1, len, f) != len)
        die("failed to write embedded package to %s", blobtar);
    fclose(f);

    char tarcmd[PATHBUF * 2];
    SNPRINTF_CHECK(tarcmd, "tar -zxf %s -C %s --strip-components=1", blobtar,
                   workdir);
    run_cmd_or_die(tarcmd, "extracting embedded CDE package");

    remove(blobtar);
}

/* Like write_and_extract(), but a tar failure is treated as "wrong
 * password" (returns 0) instead of a fatal internal error -- used only
 * for password-guess attempts, where a bad key occasionally produces
 * bytes that pass the 2-byte gzip-magic check without being valid gzip. */
static int try_extract(const char *workdir, const char *blobtar,
                        const unsigned char *data, size_t len)
{
    FILE *f = fopen(blobtar, "wb");
    if (!f)
        die("could not create %s: %s", blobtar, strerror(errno));
    int wrote_ok = (fwrite(data, 1, len, f) == len);
    fclose(f);
    if (!wrote_ok)
        die("failed to write embedded package to %s", blobtar);

    char tarcmd[PATHBUF * 2];
    SNPRINTF_CHECK(tarcmd, "tar -zxf %s -C %s --strip-components=1 2>/dev/null",
                   blobtar, workdir);
    int status = system(tarcmd);
    remove(blobtar);

    if (status == -1)
        die("failed to run command for extracting embedded CDE package: %s",
            tarcmd);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Parses "aa:bb:cc:dd:ee:ff" into 6 raw bytes. Returns 0 on bad format.
 * Must match mkblob.c's parse_mac(). */
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

/* Enumerates this machine's real network interface MAC addresses (skips
 * lo and any all-zero address). Typically 1-4 entries on a real machine. */
static int list_local_macs(unsigned char macs[][6], int max)
{
    DIR *d = opendir("/sys/class/net");
    if (!d)
        return 0;

    int count = 0;
    struct dirent *entry;
    while (count < max && (entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, "lo") == 0)
            continue;

        char path[PATHBUF];
        if (snprintf(path, sizeof path, "/sys/class/net/%s/address",
                     entry->d_name) >= (int)sizeof path)
            continue;

        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char line[64] = "";
        char *got = fgets(line, sizeof line, f);
        fclose(f);
        if (!got)
            continue;

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        unsigned char mac[6];
        if (!parse_mac(line, mac))
            continue;
        if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0 && mac[3] == 0 &&
            mac[4] == 0 && mac[5] == 0)
            continue;

        memcpy(macs[count], mac, 6);
        count++;
    }
    closedir(d);
    return count;
}

/* Parses a UUID (32 hex digits, hyphens optional/ignored) into 16 raw
 * bytes. Returns 0 on bad format. Must match mkblob.c's parse_uuid(). */
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

static int read_uuid_file(const char *path, unsigned char out[16])
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0; /* missing or (e.g. product_uuid as non-root) unreadable */
    char line[64] = "";
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got)
        return 0;

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    return parse_uuid(line, out);
}

/* Tries the machine's hardware UUID (root-only) and, since that's often
 * unreadable by an ordinary user, falls back to the OS-install UUID
 * (world-readable) -- see the "universal" concern in readme.txt: this
 * must never die()/require root, only ever try and skip on failure, or
 * a -uuid-protected blob would refuse to run for any non-root user even
 * on the licensed machine. */
static int list_local_uuids(unsigned char uuids[][16], int max)
{
    static const char *paths[] = {
        "/sys/class/dmi/id/product_uuid",
        "/etc/machine-id",
    };
    int count = 0;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && count < max;
         i++) {
        if (read_uuid_file(paths[i], uuids[count]))
            count++;
    }
    return count;
}

/* Tries to decrypt+extract with the given password against every
 * plausible MAC/UUID combination: an all-zero placeholder for each
 * (matches an unrestricted build) plus every real local candidate
 * (matches a build restricted to this machine). Returns 1 on the first
 * combination that produces a real, extractable tar archive. */
static int try_all_candidates(const char *workdir, const char *blobtar,
                               const unsigned char salt[16],
                               uint32_t iterations, const char *password,
                               const unsigned char expire_bytes[8],
                               const unsigned char *ciphertext,
                               size_t ciphertext_len,
                               unsigned char *plaintext)
{
    unsigned char base_key[32];
    derive_key(password, salt, iterations, base_key);
    for (size_t i = 0; i < sizeof base_key; i++)
        base_key[i] ^= g_tamper;

    unsigned char local_macs[MAX_LOCAL_MACS][6];
    int nmacs = list_local_macs(local_macs, MAX_LOCAL_MACS);
    unsigned char zero_mac[6] = {0};

    unsigned char local_uuids[MAX_LOCAL_UUIDS][16];
    int nuuids = list_local_uuids(local_uuids, MAX_LOCAL_UUIDS);
    unsigned char zero_uuid[16] = {0};

    int found = 0;
    for (int m = -1; m < nmacs && !found; m++) {
      for (int u = -1; u < nuuids && !found; u++) {
        const unsigned char *mac_candidate =
            (m == -1) ? zero_mac : local_macs[m];
        const unsigned char *uuid_candidate =
            (u == -1) ? zero_uuid : local_uuids[u];

        unsigned char final_key[32];
        mix_key(base_key, expire_bytes, mac_candidate, uuid_candidate,
                final_key);

        memcpy(plaintext, ciphertext, ciphertext_len);
        keystream_xor(plaintext, ciphertext_len, final_key, salt);
        explicit_bzero(final_key, sizeof final_key);

        int looks_like_gzip = (ciphertext_len >= 2 && plaintext[0] == 0x1f &&
                                plaintext[1] == 0x8b);
        if (looks_like_gzip &&
            try_extract(workdir, blobtar, plaintext, ciphertext_len))
            found = 1;
      }
    }

    explicit_bzero(base_key, sizeof base_key);
    return found;
}

/* If the embedded payload starts with ENC_MAGIC, it was encrypted by
 * mkblob.c's encrypt_payload() with a key derived from a password (see
 * sha256.h's derive_key()/keystream_xor()). This is checkpoint A: only a
 * decrypt that both looks like gzip and actually extracts with `tar`
 * counts as success. Anything else (including g_tamper corrupting the
 * key) retries up to MAX_PASSWORD_ATTEMPTS times, then gives up. Plain,
 * unprotected payloads (no ENC_MAGIC) are extracted exactly as before. */
static void extract_embedded_blob(const char *workdir)
{
    char blobtar[PATHBUF];
    SNPRINTF_CHECK(blobtar, "%s/blob_input.tar.gz", workdir);

    const unsigned char *payload = (const unsigned char *)blob_tar;
    size_t payload_len = size_tar;

    if (payload_len < ENC_HEADER_LEN ||
        memcmp(payload, ENC_MAGIC, 4) != 0) {
        write_and_extract(workdir, blobtar, payload, payload_len);
        return;
    }

    uint32_t iterations;
    memcpy(&iterations, payload + 4, sizeof iterations);
    unsigned char salt[16];
    memcpy(salt, payload + 8, sizeof salt);
    unsigned char expire_bytes[8];
    memcpy(expire_bytes, payload + 24, sizeof expire_bytes);
    unsigned char requires_password = payload[32];

    /* Expiry is stored in the clear and checked up front: no point trying
     * passwords/MACs if the license period is simply over. Like any
     * offline expiry scheme this is just a system-clock comparison --
     * patchable, and the clock itself can be turned back. It is not, and
     * cannot be, stronger than that. */
    uint64_t expire_v;
    memcpy(&expire_v, expire_bytes, sizeof expire_v);
    if (expire_v != 0 && (uint64_t)time(NULL) >= expire_v) {
        char msg[32];
        deobfuscate(msg, obf_expired, sizeof obf_expired);
        fprintf(stderr, "%s\n", msg);
        char rmcmd[PATHBUF];
        SNPRINTF_CHECK(rmcmd, "rm -rf %s", workdir);
        run_cmd(rmcmd);
        exit(EXIT_FAILURE);
    }

    const unsigned char *ciphertext = payload + ENC_HEADER_LEN;
    size_t ciphertext_len = payload_len - ENC_HEADER_LEN;

    unsigned char *plaintext = malloc(ciphertext_len);
    if (!plaintext)
        die("out of memory");

    int authenticated;
    if (!requires_password) {
        /* No password was set at build time -- MAC/expiry (if any) are
         * the only constraints, so try silently, no prompting. */
        authenticated = try_all_candidates(workdir, blobtar, salt,
                                               iterations, "", expire_bytes,
                                               ciphertext, ciphertext_len,
                                               plaintext);
        if (!authenticated) {
            char msg[64];
            deobfuscate(msg, obf_not_authorized, sizeof obf_not_authorized);
            fprintf(stderr, "%s\n", msg);
        }
    } else {
        authenticated = 0;
        for (int attempt = 0; attempt < MAX_PASSWORD_ATTEMPTS && !authenticated;
             attempt++) {
            char prompt[16];
            deobfuscate(prompt, obf_password_prompt,
                        sizeof obf_password_prompt);
            char password[256];
            read_hidden_line(prompt, password, sizeof password);

            authenticated = try_all_candidates(
                workdir, blobtar, salt, iterations, password, expire_bytes,
                ciphertext, ciphertext_len, plaintext);
            explicit_bzero(password, sizeof password);

            if (!authenticated) {
                char msg[32];
                deobfuscate(msg, obf_incorrect_password,
                            sizeof obf_incorrect_password);
                fprintf(stderr, "%s\n", msg);
            }
        }
    }

    free(plaintext);

    if (!authenticated) {
        char rmcmd[PATHBUF];
        SNPRINTF_CHECK(rmcmd, "rm -rf %s", workdir);
        run_cmd(rmcmd);
        exit(EXIT_FAILURE);
    }
}

/* Checkpoint B: independent of whatever extract_embedded_blob() decided,
 * verify the extraction actually produced real files before running
 * anything. A patch that skips checkpoint A's exit() still has to
 * contend with this one, since it can only pass if a real successful
 * extraction happened (which requires the correct key). */
static void verify_extraction_or_die(const char *workdir)
{
    char execpath[PATHBUF], rootpath[PATHBUF];
    SNPRINTF_CHECK(execpath, "%s/execute.cde", workdir);
    SNPRINTF_CHECK(rootpath, "%s/cde-root", workdir);

    if (access(execpath, F_OK) != 0 || access(rootpath, F_OK) != 0) {
        char msg[32];
        deobfuscate(msg, obf_incorrect_password, sizeof obf_incorrect_password);
        fprintf(stderr, "%s\n", msg);

        char rmcmd[PATHBUF];
        SNPRINTF_CHECK(rmcmd, "rm -rf %s", workdir);
        run_cmd(rmcmd);
        exit(EXIT_FAILURE);
    }
}

static void build_run_command(const char *workdir, int argc, char *argv[],
                               char *command, size_t command_sz)
{
    const char *path_env = getenv("PATH");
    char header[PATHBUF * 2];
    SNPRINTF_CHECK(header, "export PATH=\"%s:%s\"; execute.cde ", workdir,
                   path_env ? path_env : "");
    command[0] = '\0';
    xstrlcat(command, command_sz, header);

    for (int i = 1; i < argc; i++) {
        xstrlcat(command, command_sz, argv[i]);
        xstrlcat(command, command_sz, " ");
    }
}

int main(int argc, char *argv[])
{
    deobfuscate(progname, obf_progname, sizeof obf_progname);
    check_not_debugged();

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        die("could not determine the current directory: %s",
            strerror(errno));

    create_temp_workdir(g_workdir);
    extract_embedded_blob(g_workdir);

    char command[8192];
    build_run_command(g_workdir, argc, argv, command, sizeof command);

    char chmodcmd[PATHBUF];
    SNPRINTF_CHECK(chmodcmd, "chmod -R 755 %s", g_workdir);
    run_cmd(chmodcmd);

    SNPRINTF_CHECK(g_rm_cmd, "rm -rf %s/", g_workdir);
    SNPRINTF_CHECK(g_pidof_cmd,
                    "pidof -x %s/cde-root/lib64/ld-linux-x86-64.so.2 "
                    "> /dev/null",
                    g_workdir);

    pthread_t watcher;
    if (pthread_create(&watcher, NULL, &cleanup_watcher, NULL) != 0)
        die("could not start the cleanup watcher thread: %s",
            strerror(errno));

    verify_extraction_or_die(g_workdir);

    if (chdir(cwd) != 0)
        die("could not chdir back to %s: %s", cwd, strerror(errno));
    run_cmd(command);

    char rmdircmd[PATHBUF];
    SNPRINTF_CHECK(rmdircmd, "rm -rf %s", g_workdir);
    run_cmd(rmdircmd);

    return 0;
}
