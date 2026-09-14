/*
 * mkblobid.c - reports this machine's MAC address(es) and UUID(s) in the
 * exact format mkblob's -mac/-uuid flags expect.
 *
 * Meant to be handed to a customer: they run this on the machine a blob
 * should be locked to, and send the output back to whoever runs mkblob.
 * Standalone tool, unrelated to the mkblob/mkblobexec build pipeline --
 * compile directly:
 *   gcc -O2 -o mkblobid mkblobid.c -static
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

#define PATHBUF 512
#define MAX_MACS 16

static int looks_like_mac(const char *s)
{
    if (strlen(s) != 17)
        return 0;
    unsigned int b[6];
    return sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3],
                  &b[4], &b[5]) == 6;
}

static int looks_like_uuid(const char *s)
{
    int hexlen = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '-')
            continue;
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F')))
            return 0;
        hexlen++;
    }
    return hexlen == 32;
}

static void trim_newline(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

/* Reads the first line of a file into buf (size bufsz). Returns 1 on
 * success, 0 if the file is missing, empty, or unreadable (e.g. a
 * root-only file read as a non-root user) -- never fails loudly, since
 * this tool has to work for whoever the customer happens to be. */
static int read_first_line(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char *got = fgets(buf, (int)bufsz, f);
    fclose(f);
    if (!got)
        return 0;
    trim_newline(buf);
    return buf[0] != '\0';
}

/* Collects every real (non-lo, non-all-zero) interface MAC address. */
static int list_macs(char macs[][18], int max)
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

        char line[64];
        if (!read_first_line(path, line, sizeof line))
            continue;
        if (!looks_like_mac(line))
            continue;
        if (strcmp(line, "00:00:00:00:00:00") == 0)
            continue;

        snprintf(macs[count], 18, "%s", line);
        count++;
    }
    closedir(d);
    return count;
}

int main(void)
{
    printf("mkblobid - MAC/UUID report for use with mkblob's -mac/-uuid flags\n");
    printf("===================================================================\n\n");

    char macs[MAX_MACS][18];
    int nmacs = list_macs(macs, MAX_MACS);
    printf("Network interface MAC address(es):\n");
    if (nmacs == 0) {
        printf("  (none found)\n");
    } else {
        for (int i = 0; i < nmacs; i++)
            printf("  %s\n", macs[i]);
    }
    printf("\n");

    char product_uuid[64] = "";
    int have_product_uuid =
        read_first_line("/sys/class/dmi/id/product_uuid", product_uuid,
                         sizeof product_uuid) &&
        looks_like_uuid(product_uuid);

    printf("Hardware UUID (/sys/class/dmi/id/product_uuid):\n");
    if (have_product_uuid) {
        printf("  %s\n", product_uuid);
    } else if (geteuid() != 0) {
        printf("  Not readable as this user (needs root) - re-run this "
               "program with sudo to get it.\n");
    } else {
        printf("  Not available on this machine.\n");
    }
    printf("\n");

    char machine_id[64] = "";
    int have_machine_id = read_first_line("/etc/machine-id", machine_id,
                                           sizeof machine_id) &&
                           looks_like_uuid(machine_id);

    printf("OS-install UUID (/etc/machine-id):\n");
    printf("  %s\n\n", have_machine_id ? machine_id : "Not available on this machine.");

    printf("Example mkblob command using the values above:\n  mkblob "
           "/path/to/program -o out.blob");
    if (nmacs > 0)
        printf(" -mac %s", macs[0]);
    if (have_product_uuid)
        printf(" -uuid %s", product_uuid);
    else if (have_machine_id)
        printf(" -uuid %s", machine_id);
    printf("\n");

    if (nmacs == 0 && !have_product_uuid && !have_machine_id) {
        fprintf(stderr, "\nWarning: no usable MAC address or UUID was "
                        "found on this machine.\n");
        return 1;
    }
    return 0;
}
