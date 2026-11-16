/*
 * ept - ELKS Package Tool
 *
 * Installs and updates programs from a plain HTTP package repository.
 *
 * Index line:      name version md5 size mode dest
 * Installed line:  name version md5 dest channel       (channel is s or n)
 * File URL:        <repo>/pool/<name><dest>
 *
 * A "#min <version>" line in the index names the oldest ept that can use
 * the repository; an older ept updates itself before it installs anything.
 *
 * The nightly channel (-n) is a second repository of the same layout
 * under <repo>/nightly. Each installed package remembers its channel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "md5.c"                /* md5_t, md5_init/update/final, readbuf[BUFSIZ] */

#define VERSION     "1.3"
#define SELF        "/bin/ept"
#define DEFREPO     "http://ept.samwarr.dev"
#define CONFIG      "/etc/ept.cfg"
#define DIR         "/etc/ept"
#define INDEX       DIR "/index"
#define INDEXNEW    DIR "/index.new"
#define NINDEX      DIR "/nindex"
#define NINDEXNEW   DIR "/nindex.new"
#define DB          DIR "/inst"
#define DBNEW       DIR "/inst.new"

#define LINESZ      256
#define STALL       10          /* seconds without data before a download is retried */
#define TRIES       3
#define NIDX        6           /* fields per index line */
#define NDB         5           /* fields per installed line */

static char host[64];
static int port = 80;
static char base[96];           /* repository path prefix, no trailing slash */
static char iline[LINESZ];      /* index iteration */
static char dline[LINESZ];      /* installed list iteration */
static char sline[LINESZ];      /* nested lookups */
static char tmp[LINESZ];
static char names[1024];
static int chan;                /* 0 = stable, 1 = nightly */
static char **args;             /* argv as given, to start ept again */

#define IDX         (chan ? NINDEX : INDEX)
#define IDXNEW      (chan ? NINDEXNEW : INDEXNEW)
#define CHANDIR     (chan ? "/nightly" : "")
#define CHANNAME    (chan ? "n" : "s")

static void die(const char *msg)
{
    fprintf(stderr, "ept: %s\n", msg);
    exit(1);
}

static void usage(void)
{
    fprintf(stderr,
        "ept " VERSION " - ELKS Package Tool\n"
        "Usage: ept update                  fetch the package lists\n"
        "       ept list [-n]               show available and installed packages\n"
        "       ept install [-n] <pkg> ...  install or reinstall packages\n"
        "       ept upgrade                 update all installed packages\n"
        "       ept remove <pkg> ...        remove packages\n"
        "  -n   use the nightly builds instead of the tagged releases\n");
    exit(1);
}

/* split line in place at whitespace, returns field count */
static int split(char *s, char **f, int max)
{
    int n = 0;

    while (n < max) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '\n' || *s == '\r')
            break;
        f[n++] = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
            s++;
        if (*s)
            *s++ = '\0';
    }
    return n;
}

static void chomp(char *s)
{
    char *p = s + strlen(s);

    while (p > s && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '))
        *--p = '\0';
}

static void set_repo(char *url)
{
    char *p;
    unsigned int n;

    chomp(url);
    if (strncmp(url, "http://", 7) != 0)
        die("repository must be an http:// URL");
    url += 7;
    for (p = url; *p && *p != ':' && *p != '/'; p++)
        ;
    n = p - url;
    if (n == 0 || n >= sizeof(host))
        die("bad repository host");
    memcpy(host, url, n);
    host[n] = '\0';
    port = 80;
    if (*p == ':') {
        port = atoi(++p);
        while (*p && *p != '/')
            p++;
    }
    if (strlen(p) >= sizeof(base))
        die("repository path too long");
    strcpy(base, p);
    n = strlen(base);
    while (n > 0 && base[n - 1] == '/')
        base[--n] = '\0';
}

static void read_config(void)
{
    FILE *fp;

    strcpy(tmp, DEFREPO);
    set_repo(tmp);
    if ((fp = fopen(CONFIG, "r")) == NULL)
        return;
    while (fgets(tmp, sizeof(tmp), fp))
        if (strncmp(tmp, "REPO=", 5) == 0)
            set_repo(tmp + 5);
    fclose(fp);
}

static int net_connect(void)
{
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    addr.sin_family = AF_INET;
    addr.sin_port = PORT_ANY;
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto error;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = in_gethostbyname(host);
    if (!addr.sin_addr.s_addr)
        goto error;
    if (in_connect(fd, (struct sockaddr *)&addr, sizeof(addr), 10) < 0)
        goto error;
    return fd;
error:
    close(fd);
    return -1;
}

/*
 * Redraw the download meter in place: a bar when the size is known,
 * otherwise a running KB count. Returns what was drawn, so callers
 * only write again when it changes (the ELKS console is slow).
 */
static int meter(long got, long size, int last)
{
    char buf[40];
    int v, i, n;

    if (size > 0) {
        if (got > size)
            got = size;
        v = (int)(got * 100 / size);
        if (v == last)
            return last;
        strcpy(buf, "\r  [");
        for (i = 0; i < 20; i++)
            buf[4 + i] = i < v / 5 ? '#' : '.';
        n = 24 + sprintf(buf + 24, "] %3d%%", v);
    } else {
        v = (int)(got >> 10);
        if (v == last)
            return last;
        n = sprintf(buf, "\r  %dK", v);
    }
    write(STDOUT_FILENO, buf, n);
    return v;
}

/*
 * GET <repo><rpath> into outfile. On success returns 0 and sets *size and the
 * hex md5 of the body. Anything but a complete HTTP 200 reply is an error.
 * Returns -2 when the server went quiet, which is worth another attempt.
 * expect is the size from the index, for the progress bar; 0 if unknown.
 */
static int http_try(const char *rpath, const char *outfile, char *hexsum, long *size,
    long expect)
{
    int fd, out, n, i, hdr = 1, nl = 0, slen = 0, rc = -1, shown = -1;
    long total = 0;
    char status[13];
    unsigned char digest[16];
    md5_t ctx;
    char *req = (char *)readbuf;

    if (strlen(base) + strlen(rpath) + strlen(host) > 400) {
        fprintf(stderr, "ept: URL too long\n");
        return -1;
    }
    fflush(stdout);                     /* meter uses write(); keep it in order */
    fd = net_connect();
    if (fd < 0) {
        fprintf(stderr, "ept: cannot connect to %s port %d (is the network up?)\n",
            host, port);
        return -1;
    }
    if (port == 80)
        sprintf(req, "GET %s%s HTTP/1.0\r\nHost: %s\r\n", base, rpath, host);
    else
        sprintf(req, "GET %s%s HTTP/1.0\r\nHost: %s:%d\r\n", base, rpath, host, port);
    strcat(req, "User-Agent: ept/" VERSION "\r\nConnection: close\r\n\r\n");
    n = strlen(req);
    if (write(fd, req, n) != n) {
        fprintf(stderr, "ept: send failed\n");
        close(fd);
        return -1;
    }
    out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        perror(outfile);
        close(fd);
        return -1;
    }
    md5_init(&ctx);
    for (;;) {
        fd_set rfds;
        struct timeval tv;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = STALL;
        tv.tv_usec = 0;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) == 0) {
            if (shown >= 0)
                write(STDOUT_FILENO, "\n", 1);
            fprintf(stderr, "ept: no data from server for %d seconds\n", STALL);
            rc = -2;
            goto done;
        }
        n = read(fd, readbuf, sizeof(readbuf));
        if (n <= 0)
            break;
        i = 0;
        if (hdr) {
            for (; i < n && hdr; i++) {
                char c = readbuf[i];

                if (slen < 12)
                    status[slen++] = c;
                if (c == '\n') {
                    if (nl)
                        hdr = 0;        /* blank line ends the header */
                    nl = 1;
                } else if (c != '\r')
                    nl = 0;
            }
            if (!hdr) {
                status[slen] = '\0';
                if (strncmp(status, "HTTP/1.", 7) != 0 || strncmp(status + 9, "200", 3) != 0) {
                    status[12] = '\0';
                    fprintf(stderr, "ept: server answered '%s' for %s%s\n",
                        slen >= 12 ? status + 9 : "?", base, rpath);
                    goto done;
                }
            }
        }
        if (i < n) {
            if (write(out, readbuf + i, n - i) != n - i) {
                perror(outfile);
                goto done;
            }
            md5_update(&ctx, readbuf + i, n - i);
            total += n - i;
            if (expect > 0 || total >= 8192)    /* progress for slow machines */
                shown = meter(total, expect, shown);
        }
    }
    if (shown >= 0)
        write(STDOUT_FILENO, "\n", 1);
    if (n < 0)
        fprintf(stderr, "ept: read error from server\n");
    else if (hdr)
        fprintf(stderr, "ept: incomplete reply from server\n");
    else
        rc = 0;
done:
    close(fd);
    if (close(out) < 0)
        rc = -1;
    if (rc != 0) {
        unlink(outfile);
        return rc;
    }
    md5_final(&ctx, digest);
    for (i = 0; i < 16; i++)
        sprintf(hexsum + i * 2, "%02x", digest[i]);
    *size = total;
    return 0;
}

/* http_try, started again on a new connection when a transfer stalls */
static int http_get(const char *rpath, const char *outfile, char *hexsum, long *size,
    long expect)
{
    int rc, try;

    for (try = 1; ; try++) {
        rc = http_try(rpath, outfile, hexsum, size, expect);
        if (rc != -2 || try == TRIES)
            return rc;
        fprintf(stderr, "ept: trying again (%d of %d)\n", try + 1, TRIES);
    }
}

/* replace file 'to' with file 'from'; copies where rename is unsupported (FAT) */
static int replace(const char *from, const char *to)
{
    int in, out, n, rc = -1;

    if (rename(from, to) == 0)
        return 0;
    /*
     * No rename onto an existing file here (FAT). Remove the old file first:
     * a running program (ept itself) can be unlinked but not overwritten.
     */
    unlink(to);
    if (rename(from, to) == 0)
        return 0;
    if ((in = open(from, O_RDONLY)) < 0) {
        perror(from);
        return -1;
    }
    if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        perror(to);
        close(in);
        return -1;
    }
    while ((n = read(in, readbuf, sizeof(readbuf))) > 0)
        if (write(out, readbuf, n) != n) {
            n = -1;
            break;
        }
    close(in);
    if (close(out) == 0 && n == 0)
        rc = 0;
    if (rc != 0)
        perror(to);
    else
        unlink(from);
    return rc;
}

/* create the parent directories of path */
static void mkdirs(char *path)
{
    char *p;

    for (p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
        }
    }
}

/* is this exact file record in the installed list? */
static int isinst(char *name, char *ver, char *sum, char *dest)
{
    FILE *fp;
    char *f[NDB];
    int found = 0;

    if ((fp = fopen(DB, "r")) == NULL)
        return 0;
    while (!found && fgets(sline, sizeof(sline), fp))
        if (split(sline, f, NDB) == NDB && !strcmp(f[3], dest) && !strcmp(f[0], name)
                && !strcmp(f[1], ver) && !strcmp(f[2], sum))
            found = 1;
    fclose(fp);
    return found;
}

/*
 * copy the installed version of package name to ver;
 * returns 0 if not installed, 1 if stable, 2 if nightly
 */
static int instver(const char *name, char *ver)
{
    FILE *fp;
    char *f[NDB];
    int found = 0;

    if ((fp = fopen(DB, "r")) == NULL)
        return 0;
    while (!found && fgets(sline, sizeof(sline), fp))
        if (split(sline, f, NDB) == NDB && !strcmp(f[0], name)) {
            strcpy(ver, f[1]);
            found = (f[4][0] == 'n') ? 2 : 1;
        }
    fclose(fp);
    return found;
}

/* does the index list dest as part of package name? any file if dest is NULL */
static int inindex(const char *name, const char *dest)
{
    FILE *fp;
    char *f[NIDX];
    int found = 0;

    if ((fp = fopen(IDX, "r")) == NULL)
        return 0;
    while (!found && fgets(sline, sizeof(sline), fp))
        if (split(sline, f, NIDX) == NIDX && !strcmp(f[0], name)
                && (!dest || !strcmp(f[5], dest)))
            found = 1;
    fclose(fp);
    return found;
}

/* fetch the package list of channel c */
static int update(int c)
{
    FILE *fp;
    char *f[NIDX];
    char sum[33], prev[64];
    long size;
    int first = 1, ok = 0, count = 0;

    chan = c;
    mkdir(DIR, 0755);
    sprintf(sline, "%s/index", CHANDIR);
    if (port == 80)
        printf("Fetching http://%s%s%s\n", host, base, sline);
    else
        printf("Fetching http://%s:%d%s%s\n", host, port, base, sline);
    if (http_get(sline, IDXNEW, sum, &size, 0) != 0)
        return 1;
    if ((fp = fopen(IDXNEW, "r")) == NULL) {
        perror(IDXNEW);
        return 1;
    }
    prev[0] = '\0';
    while (fgets(iline, sizeof(iline), fp)) {
        chomp(iline);
        if (first) {
            first = 0;
            if (strcmp(iline, "#ept1") != 0)
                break;
            continue;
        }
        ok = !strcmp(iline, "#end");        /* must be the last line */
        if (iline[0] == '#')
            continue;
        if (split(iline, f, NIDX) == NIDX && strcmp(f[0], prev) != 0) {
            strncpy(prev, f[0], sizeof(prev) - 1);
            prev[sizeof(prev) - 1] = '\0';
            count++;
        }
    }
    fclose(fp);
    if (!ok) {
        unlink(IDXNEW);
        fprintf(stderr, "ept: server did not return a valid package list\n");
        return 1;
    }
    if (replace(IDXNEW, IDX) != 0)
        return 1;
    printf("%d %spackages available\n", count, chan ? "nightly " : "");
    return 0;
}

/* download, verify and install one file of a package */
static int getfile(char **f)
{
    char *name = f[0], *sum = f[2], *dest = f[5];
    char *p;
    char got[33];
    long size;

    if (dest[0] != '/' || strlen(dest) > LINESZ - 16) {
        fprintf(stderr, "ept: %s: bad path in package list\n", dest);
        return 1;
    }
    printf("  %s (%s bytes)\n", dest, f[3]);
    strcpy(tmp, dest);
    mkdirs(tmp);
    p = strrchr(tmp, '/');
    strcpy(p + 1, "ept.tmp");

    /* sline is free here: nested lookups are done before getfile is called */
    sprintf(sline, "%s/pool/%s%s", CHANDIR, name, dest);
    if (!strcmp(f[3], "0")) {           /* an empty file needs no download */
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd < 0 || close(fd) < 0) {
            perror(tmp);
            return 1;
        }
    } else {
        if (http_get(sline, tmp, got, &size, atol(f[3])) != 0)
            return 1;
        if (size != atol(f[3]) || strcmp(got, sum) != 0) {
            unlink(tmp);
            fprintf(stderr, "ept: %s: checksum mismatch, file not installed\n", dest);
            return 1;
        }
    }
    if (replace(tmp, dest) != 0) {
        unlink(tmp);
        return 1;
    }
    chmod(dest, (mode_t)strtol(f[4], NULL, 8));
    return 0;
}

/* install or update one package from channel c: 0 ok, 1 failed, 2 not in the index */
static int ipkg(const char *pkg, int c)
{
    FILE *ix, *db, *out;
    char *f[NIDX];
    char ver[64];
    int found = 0, got = 0, bad = 0;

    chan = c;
    if ((ix = fopen(IDX, "r")) == NULL) {
        perror(IDX);
        return 1;
    }
    while (fgets(iline, sizeof(iline), ix)) {
        if (split(iline, f, NIDX) != NIDX || strcmp(f[0], pkg) != 0)
            continue;
        found = 1;
        strncpy(ver, f[1], sizeof(ver) - 1);
        ver[sizeof(ver) - 1] = '\0';
        if (isinst(f[0], f[1], f[2], f[5]) && access(f[5], 0) == 0)
            continue;
        if (!got)
            printf("Installing %s %s%s\n", pkg, ver, chan ? " (nightly)" : "");
        if (getfile(f) != 0) {
            bad = 1;
            break;
        }
        got++;
    }
    if (!found) {
        fclose(ix);
        return 2;
    }
    if (bad) {
        fclose(ix);
        return 1;
    }

    /* rebuild the installed list, deleting files the new version dropped */
    if ((out = fopen(DBNEW, "w")) == NULL) {
        perror(DBNEW);
        fclose(ix);
        return 1;
    }
    if ((db = fopen(DB, "r")) != NULL) {
        while (fgets(dline, sizeof(dline), db)) {
            if (split(dline, f, NDB) != NDB)
                continue;
            if (strcmp(f[0], pkg) != 0)
                fprintf(out, "%s %s %s %s %s\n", f[0], f[1], f[2], f[3], f[4]);
            else if (!inindex(pkg, f[3])) {
                printf("  removing %s\n", f[3]);
                unlink(f[3]);
            }
        }
        fclose(db);
    }
    rewind(ix);
    while (fgets(iline, sizeof(iline), ix))
        if (split(iline, f, NIDX) == NIDX && !strcmp(f[0], pkg))
            fprintf(out, "%s %s %s %s %s\n", f[0], f[1], f[2], f[5], CHANNAME);
    fclose(ix);
    if (fclose(out) != 0 || replace(DBNEW, DB) != 0) {
        fprintf(stderr, "ept: cannot update %s\n", DB);
        return 1;
    }
    if (!got)
        printf("%s %s is up to date\n", pkg, ver);
    return 0;
}

/* compare dotted version numbers: <0, 0 or >0 like strcmp */
static int vercmp(const char *a, const char *b)
{
    long x, y;

    for (;;) {
        x = strtol(a, (char **)&a, 10);
        y = strtol(b, (char **)&b, 10);
        if (x != y)
            return x < y ? -1 : 1;
        if (*a != '.' && *b != '.')
            return 0;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
}

/*
 * If the package list of channel c asks for a newer ept than this one,
 * install the ept package and start it again to finish off the install
 */
static void selfupdate(int c)
{
    FILE *fp;
    char *f[2];
    int need = 0;

    chan = c;
    if ((fp = fopen(IDX, "r")) == NULL)
        return;
    /* "#min <version>" is one of the comment lines before the first package */
    while (!need && fgets(sline, sizeof(sline), fp) && sline[0] == '#')
        if (split(sline, f, 2) == 2 && !strcmp(f[0], "#min") && vercmp(VERSION, f[1]) < 0)
            need = 1;
    fclose(fp);
    if (!need)
        return;
    printf("This repository needs ept %s, this is ept " VERSION ": updating ept first\n", f[1]);
    /* EPTSELF is set when this already is the updated ept */
    if (getenv("EPTSELF") || ipkg("ept", c) != 0) {
        fprintf(stderr, "ept: cannot update ept itself, install a newer " SELF " by hand\n");
        exit(1);
    }
    putenv("EPTSELF=1");
    fflush(stdout);
    execv(SELF, args);
    perror(SELF);
    exit(1);
}

static int install(int argc, char **argv, int c)
{
    int i, rc = 0;

    if (argc < 1)
        usage();
    if (update(c) != 0)                 /* never install from a stale list */
        return 1;
    selfupdate(c);
    for (i = 0; i < argc; i++) {
        switch (ipkg(argv[i], c)) {
        case 1:
            rc = 1;
            break;
        case 2:
            fprintf(stderr, "ept: %s: no such package\n", argv[i]);
            rc = 1;
            break;
        }
    }
    return rc;
}

static int upgrade(void)
{
    FILE *db;
    char *f[NDB];
    char *p, *last = NULL;
    int rc = 0, nightly = 0;

    if (update(0) != 0)
        return 1;
    selfupdate(0);
    names[0] = '\0';
    p = names;
    if ((db = fopen(DB, "r")) != NULL) {
        while (fgets(dline, sizeof(dline), db)) {
            if (split(dline, f, NDB) != NDB)
                continue;
            if (last && !strcmp(last, f[0]))
                continue;
            if (p + strlen(f[0]) + 3 > names + sizeof(names)) {
                fprintf(stderr, "ept: too many packages, some not checked\n");
                rc = 1;
                break;
            }
            *p++ = f[4][0];             /* channel letter, then the name */
            if (f[4][0] == 'n')
                nightly = 1;
            last = p;
            strcpy(p, f[0]);
            p += strlen(p) + 1;
            *p = '\0';                  /* list ends with an empty entry */
        }
        fclose(db);
    }
    if (nightly && update(1) != 0)
        return 1;
    if (nightly)
        selfupdate(1);
    for (p = names; *p; p += strlen(p) + 1) {
        switch (ipkg(p + 1, *p == 'n')) {
        case 1:
            rc = 1;
            break;
        case 2:
            printf("%s is no longer in the repository, keeping it\n", p + 1);
            break;
        }
    }
    return rc;
}

static int rmpkg(const char *pkg)
{
    FILE *db, *out;
    char *f[NDB];
    char ver[64];

    if (!instver(pkg, ver)) {
        fprintf(stderr, "ept: %s is not installed\n", pkg);
        return 1;
    }
    printf("Removing %s %s\n", pkg, ver);
    if ((db = fopen(DB, "r")) == NULL || (out = fopen(DBNEW, "w")) == NULL) {
        perror(DB);
        return 1;
    }
    while (fgets(dline, sizeof(dline), db)) {
        if (split(dline, f, NDB) != NDB)
            continue;
        if (!strcmp(f[0], pkg))
            unlink(f[3]);
        else
            fprintf(out, "%s %s %s %s %s\n", f[0], f[1], f[2], f[3], f[4]);
    }
    fclose(db);
    if (fclose(out) != 0 || replace(DBNEW, DB) != 0)
        return 1;
    return 0;
}

static int list(int c)
{
    FILE *ix, *db;
    char *f[NIDX];
    char prev[64], ver[64];
    int inst;

    if (update(c) != 0) {
        /* no network: the saved list is still worth showing */
        chan = c;
        if (access(IDX, 0) != 0)
            return 1;
        printf("Showing the saved package list\n");
    }
    if ((ix = fopen(IDX, "r")) == NULL) {
        perror(IDX);
        return 1;
    }
    prev[0] = '\0';
    while (fgets(iline, sizeof(iline), ix)) {
        if (iline[0] == '#' || split(iline, f, NIDX) != NIDX || !strcmp(f[0], prev))
            continue;
        strncpy(prev, f[0], sizeof(prev) - 1);
        prev[sizeof(prev) - 1] = '\0';
        inst = instver(f[0], ver);
        if (!inst)
            printf("%-14s %s\n", f[0], f[1]);
        else if (!strcmp(ver, f[1]) && (inst == 2) == c)
            printf("%-14s %-12s [installed]\n", f[0], f[1]);
        else
            printf("%-14s %-12s [installed %s%s]\n", f[0], f[1], ver,
                inst == 2 ? " nightly" : "");
    }
    fclose(ix);

    /* installed packages this list does not have from any other channel */
    if ((db = fopen(DB, "r")) != NULL) {
        prev[0] = '\0';
        while (fgets(dline, sizeof(dline), db)) {
            if (split(dline, f, NDB) != NDB || !strcmp(f[0], prev))
                continue;
            strncpy(prev, f[0], sizeof(prev) - 1);
            prev[sizeof(prev) - 1] = '\0';
            if (!inindex(f[0], NULL))
                printf("%-14s %-12s [installed %s%s]\n", f[0], "-", f[1],
                    f[4][0] == 'n' ? " nightly" : "");
        }
        fclose(db);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i, rc = 0, nightly = 0;
    char *cmd;

    if (argc < 2)
        usage();
    args = argv;
    cmd = argv[1];
    if (argc > 2 && !strcmp(argv[2], "-n")) {
        nightly = 1;
        argv++;
        argc--;
    }
    read_config();
    if (!strcmp(cmd, "update")) {
        rc = update(0);
        if (rc == 0 && access(NINDEX, 0) == 0)      /* nightly channel in use */
            rc = update(1);
        return rc;
    }
    if (!strcmp(cmd, "list"))
        return list(nightly);
    if (!strcmp(cmd, "install"))
        return install(argc - 2, argv + 2, nightly);
    if (!strcmp(cmd, "upgrade"))
        return upgrade();
    if (!strcmp(cmd, "remove")) {
        if (argc < 3)
            usage();
        for (i = 2; i < argc; i++)
            rc |= rmpkg(argv[i]);
        return rc;
    }
    usage();
    return 1;
}
