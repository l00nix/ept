/*
 * ept - ELKS Package Tool
 *
 * Copyright (c) 2026 Sam Warr
 * SPDX-License-Identifier: MIT
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
 *
 * Every REPO= line of the config file is a repository. A package comes from
 * the last one listed that has it, except ept itself: only the first
 * repository can update ept or install a file at its path.
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

#define VERSION     "1.5"
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
#define MAXREPO     8           /* one bit each in live[] */
#define URLSZ       160

static char host[64];
static int port = 80;
static char base[96];           /* repository path prefix, no trailing slash */
static char iline[LINESZ];      /* index iteration */
static char dline[LINESZ];      /* installed list iteration */
static char sline[LINESZ];      /* nested lookups */
static char tmp[LINESZ];
static char names[1024];
static int chan;                /* 0 = stable, 1 = nightly */
static char repos[MAXREPO][URLSZ];  /* in config file order, the first is the primary */
static int nrepo;
static int repo;                /* the repository host, port and base are set for */
static unsigned char live[2];   /* per channel: repositories with a usable package list */
static char idxname[sizeof(NINDEX) + 4];
static int quiet;               /* a 404 is no error: this repository has no such list */
static int notfound;            /* the last reply was a 404 */
static char **args;             /* argv as given, to start ept again */

#define IDX         idx()
#define IDXNEW      (chan ? NINDEXNEW : INDEXNEW)
#define CHANDIR     (chan ? "/nightly" : "")
#define CHANNAME    (chan ? "n" : "s")
#define KB(n)       (((n) + 1023) / 1024)   /* bytes to K, rounded up */

static void die(const char *msg)
{
    fprintf(stderr, "ept: %s\n", msg);
    exit(1);
}

/* package list file of the current repository and channel */
static char *idx(void)
{
    sprintf(idxname, repo ? "%s.%d" : "%s", chan ? NINDEX : INDEX, repo);
    return idxname;
}

static void usage(void)
{
    fprintf(stderr,
        "ept " VERSION " - ELKS Package Tool\n"
        "Usage: ept update                  fetch the package lists\n"
        "       ept list [-n]               show available and installed packages\n"
        "       ept install [-n] <pkg> ...  install or reinstall packages\n"
        "       ept upgrade [-y]            update all installed packages\n"
        "       ept remove <pkg> ...        remove packages\n"
        "  -n   use the nightly builds instead of the tagged releases\n"
        "  -y   do not ask before upgrading\n");
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

static void userepo(int i)
{
    repo = i;
    set_repo(repos[i]);
}

static void read_config(void)
{
    FILE *fp;

    if ((fp = fopen(CONFIG, "r")) != NULL) {
        while (fgets(tmp, sizeof(tmp), fp)) {
            if (strncmp(tmp, "REPO=", 5) != 0)
                continue;
            chomp(tmp);
            if (nrepo == MAXREPO)
                die("too many repositories in " CONFIG);
            if (strlen(tmp + 5) >= URLSZ)
                die("repository URL too long");
            strcpy(repos[nrepo], tmp + 5);
            userepo(nrepo++);           /* stops here if the URL is no good */
        }
        fclose(fp);
    } else if ((fp = fopen(CONFIG, "w")) != NULL) {
        /* first run: leave a file to edit, the same as ept.cfg in the source */
        fputs("# " CONFIG " - ept repositories, one REPO= line each.\n"
            "#\n"
            "# WARNING: every repository listed here can install and replace\n"
            "# files anywhere on this system. Only add repositories you trust.\n"
            "#\n"
            "# The FIRST repository is the primary: it is the only one that can\n"
            "# update ept itself. Add extra repositories BELOW it; for any other\n"
            "# package, a later line overrides an earlier one.\n"
            "\n"
            "REPO=" DEFREPO "\n", fp);
        fclose(fp);
    }
    if (nrepo == 0)
        strcpy(repos[nrepo++], DEFREPO);
    userepo(0);
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
    notfound = 0;
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
                    notfound = slen >= 12 && !strncmp(status + 9, "404", 3);
                    if (!(notfound && quiet))
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

/*
 * Select the repository that provides pkg on the current channel: the last
 * one listed that has it, but ept itself only ever comes from the first.
 * Returns its number, or -1 if no usable package list has the package.
 */
static int findrepo(const char *pkg)
{
    int i;

    for (i = strcmp(pkg, "ept") ? nrepo - 1 : 0; i >= 0; i--) {
        if (!(live[chan] & (1 << i)))
            continue;
        repo = i;
        if (inindex(pkg, NULL)) {
            userepo(i);
            return i;
        }
    }
    return -1;
}

/*
 * Fetch the package list of channel c from the current repository:
 * 0 ok, 1 failed, 2 the repository has no nightly builds
 */
static int update(int c)
{
    FILE *fp;
    char sum[33];
    long size;
    int first = 1, ok = 0;

    chan = c;
    mkdir(DIR, 0755);
    sprintf(sline, "%s/index", CHANDIR);
    if (port == 80)
        printf("Fetching http://%s%s%s\n", host, base, sline);
    else
        printf("Fetching http://%s:%d%s%s\n", host, port, base, sline);
    quiet = c;
    ok = http_get(sline, IDXNEW, sum, &size, 0);
    quiet = 0;
    if (ok != 0)
        return c && notfound ? 2 : 1;
    ok = 0;
    if ((fp = fopen(IDXNEW, "r")) == NULL) {
        perror(IDXNEW);
        return 1;
    }
    while (fgets(iline, sizeof(iline), fp)) {
        chomp(iline);
        if (first) {
            first = 0;
            if (strcmp(iline, "#ept1") != 0)
                break;
            continue;
        }
        ok = !strcmp(iline, "#end");        /* must be the last line */
    }
    fclose(fp);
    if (!ok) {
        unlink(IDXNEW);
        fprintf(stderr, "ept: server did not return a valid package list\n");
        return 1;
    }
    if (replace(IDXNEW, IDX) != 0)
        return 1;
    return 0;
}

/*
 * Fetch the package lists of channel c from every repository. One that
 * fails is left out of this run; returns 1 if none could be fetched.
 */
static int fetchall(int c)
{
    int i, rc, none = 0;

    for (i = 0; i < nrepo; i++) {
        userepo(i);
        rc = update(c);
        if (rc == 0)
            live[c] |= 1 << i;
        else if (rc == 2)
            none++;
        else if (nrepo > 1)
            fprintf(stderr, "ept: skipping %s%s\n", repos[i], c ? " (nightly)" : "");
    }
    if (none == nrepo)
        fprintf(stderr, "ept: no nightly builds in %s\n",
            nrepo > 1 ? "any repository" : repos[0]);
    return live[c] ? 0 : 1;
}

/* has any repository been used for nightly builds? */
static int usesnightly(void)
{
    int found = 0;

    chan = 1;
    for (repo = 0; repo < nrepo && !found; repo++)
        found = access(IDX, 0) == 0;
    repo = 0;
    return found;
}

/*
 * Show the installed packages that have another version in their package list
 * and return how many there are. They are also left in names, each as its
 * channel letter followed by the name, with an empty entry at the end.
 */
static int pending(void)
{
    FILE *db, *ix;
    char *d[NDB], *f[NIDX];
    char *p = names;
    char prev[64], ver[64];
    long size, total = 0;
    int n = 0, nn = 0, found;

    names[0] = '\0';

    if ((db = fopen(DB, "r")) != NULL) {
        prev[0] = '\0';
        while (fgets(dline, sizeof(dline), db)) {
            if (split(dline, d, NDB) != NDB || !strcmp(d[0], prev))
                continue;
            strncpy(prev, d[0], sizeof(prev) - 1);
            prev[sizeof(prev) - 1] = '\0';
            chan = d[4][0] == 'n';
            if (findrepo(d[0]) < 0 || (ix = fopen(IDX, "r")) == NULL)
                continue;
            /* the lines of a package follow each other: version and total size */
            found = 0;
            size = 0;
            while (fgets(sline, sizeof(sline), ix)) {
                if (split(sline, f, NIDX) != NIDX || strcmp(f[0], d[0]) != 0) {
                    if (found)
                        break;
                    continue;
                }
                if (!found) {
                    strncpy(ver, f[1], sizeof(ver) - 1);
                    ver[sizeof(ver) - 1] = '\0';
                    found = 1;
                }
                size += atol(f[3]);
            }
            fclose(ix);
            if (found && strcmp(ver, d[1]) != 0) {
                if (p + strlen(d[0]) + 3 > names + sizeof(names)) {
                    fprintf(stderr, "ept: too many packages, some not checked\n");
                    break;
                }
                *p++ = d[4][0];
                strcpy(p, d[0]);
                p += strlen(p) + 1;
                *p = '\0';
                printf("  %-14s %s -> %s  %ldK%s\n", d[0], d[1], ver, KB(size),
                    chan ? " (nightly)" : "");
                total += size;
                n++;
                nn += chan;
            }
        }
        fclose(db);
    }
    if (n == 0)
        printf("All packages are up to date\n");
    else {
        printf("%d package update%s available", n, n == 1 ? "" : "s");
        if (nn)
            printf(" (%d nightly)", nn);
        printf(", up to %ldK to download\n", KB(total));
    }
    return n;
}

/* is p anything but an absolute path without empty, "." or ".." parts? */
static int badpath(const char *p)
{
    const char *q;
    int n;

    if (*p != '/')
        return 1;
    while (*p) {
        q = ++p;
        while (*p && *p != '/')
            p++;
        n = p - q;
        if (n == 0 || (q[0] == '.' && (n == 1 || (n == 2 && q[1] == '.'))))
            return 1;
    }
    return 0;
}

/* download, verify and install one file of a package */
static int getfile(char **f)
{
    char *name = f[0], *sum = f[2], *dest = f[5];
    char *p;
    char got[33];
    long size;

    if (badpath(dest) || strlen(dest) > LINESZ - 16) {
        fprintf(stderr, "ept: %s: bad path in package list\n", dest);
        return 1;
    }
    if (repo && !strcasecmp(dest, SELF)) {      /* any case: FAT has only one */
        fprintf(stderr, "ept: %s: only the first repository can install this file\n", dest);
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

/* install or update one package from channel c: 0 ok, 1 failed, 2 not in any package list */
static int ipkg(const char *pkg, int c)
{
    FILE *ix, *db, *out;
    char *f[NIDX];
    char ver[64];
    int got = 0, bad = 0;

    chan = c;
    if (findrepo(pkg) < 0)
        return 2;
    if ((ix = fopen(IDX, "r")) == NULL) {
        perror(IDX);
        return 1;
    }
    while (fgets(iline, sizeof(iline), ix)) {
        if (split(iline, f, NIDX) != NIDX || strcmp(f[0], pkg) != 0)
            continue;
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
 * If a package list of channel c asks for a newer ept than this one, install
 * the ept package and start it again to finish off. An extra repository that
 * asks for more than the first one can supply is left out instead.
 */
static void selfupdate(int c)
{
    FILE *fp;
    char *f[2];
    char min[16];
    int i;

    for (i = 0; i < nrepo; i++) {
        if (!(live[c] & (1 << i)))
            continue;
        chan = c;
        repo = i;
        if ((fp = fopen(IDX, "r")) == NULL)
            continue;
        /* "#min <version>" is one of the comment lines before the first package */
        min[0] = '\0';
        while (!min[0] && fgets(sline, sizeof(sline), fp) && sline[0] == '#')
            if (split(sline, f, 2) == 2 && !strcmp(f[0], "#min") && vercmp(VERSION, f[1]) < 0) {
                strncpy(min, f[1], sizeof(min) - 1);
                min[sizeof(min) - 1] = '\0';
            }
        fclose(fp);
        if (!min[0])
            continue;
        /* EPTSELF is set when this already is the updated ept */
        if (!getenv("EPTSELF")) {
            printf("%s needs ept %s, this is ept " VERSION ": updating ept first\n",
                repos[i], min);
            if (ipkg("ept", c) == 0) {
                putenv("EPTSELF=1");
                fflush(stdout);
                execv(SELF, args);
                perror(SELF);
                exit(1);
            }
        }
        if (i == 0) {
            fprintf(stderr, "ept: cannot update ept itself, install a newer " SELF " by hand\n");
            exit(1);
        }
        fprintf(stderr, "ept: skipping %s: it needs ept %s, the first repository has none newer\n",
            repos[i], min);
        live[c] &= ~(1 << i);
    }
}

static int install(int argc, char **argv, int c)
{
    int i, rc = 0;

    if (argc < 1)
        usage();
    if (fetchall(c) != 0)               /* never install from a stale list */
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

/* update the installed packages that have a new version, after asking unless yes is set */
static int upgrade(int yes)
{
    char *p;
    int rc = 0;

    if (fetchall(0) != 0)
        return 1;
    selfupdate(0);
    if (usesnightly()) {
        if (fetchall(1) != 0)
            return 1;
        selfupdate(1);
    }
    if (pending() == 0)
        return 0;
    if (!yes) {
        printf("Upgrade? [Y/n] ");
        fflush(stdout);
        if (fgets(tmp, sizeof(tmp), stdin) == NULL
                || (tmp[0] != 'y' && tmp[0] != 'Y' && tmp[0] != '\n')) {
            printf("Nothing upgraded\n");
            return 0;
        }
    }
    for (p = names; *p; p += strlen(p) + 1)
        if (ipkg(p + 1, *p == 'n') != 0)
            rc = 1;
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
    char prev[64], ver[64], avail[64];
    long size = 0;
    int i, rc, inst, more, show = 0, none = 0;

    for (i = 0; i < nrepo; i++) {
        userepo(i);
        rc = update(c);
        if (rc == 2) {
            none++;
            continue;
        }
        if (rc != 0) {
            /* no network: the saved list is still worth showing */
            chan = c;
            if (access(IDX, 0) != 0)
                continue;
            printf("Showing the saved package list\n");
        }
        live[c] |= 1 << i;
    }
    if (none == nrepo)
        fprintf(stderr, "ept: no nightly builds in %s\n",
            nrepo > 1 ? "any repository" : repos[0]);
    if (!live[c])
        return 1;
    chan = c;
    for (i = 0; i < nrepo; i++) {
        repo = i;
        if (!(live[c] & (1 << i)) || (ix = fopen(IDX, "r")) == NULL)
            continue;
        /* one line per package, printed after its last file so the size is complete */
        prev[0] = '\0';
        for (;;) {
            more = fgets(iline, sizeof(iline), ix) != NULL;
            if (more && (iline[0] == '#' || split(iline, f, NIDX) != NIDX))
                continue;
            if (!more || strcmp(f[0], prev) != 0) {
                if (prev[0] && show) {
                    inst = instver(prev, ver);
                    printf("%-14s %-12s %5ldK", prev, avail, KB(size));
                    if (inst && !strcmp(ver, avail) && (inst == 2) == c)
                        printf("  [installed]");
                    else if (inst)
                        printf("  [installed %s%s]", ver, inst == 2 ? " nightly" : "");
                    printf("\n");
                }
                if (!more)
                    break;
                strncpy(prev, f[0], sizeof(prev) - 1);
                prev[sizeof(prev) - 1] = '\0';
                strncpy(avail, f[1], sizeof(avail) - 1);
                avail[sizeof(avail) - 1] = '\0';
                size = 0;
                show = findrepo(prev) == i;     /* not if another repository overrides it */
            }
            size += atol(f[3]);
        }
        fclose(ix);
    }

    /* installed packages these lists do not have from any other channel */
    if ((db = fopen(DB, "r")) != NULL) {
        prev[0] = '\0';
        while (fgets(dline, sizeof(dline), db)) {
            if (split(dline, f, NDB) != NDB || !strcmp(f[0], prev))
                continue;
            strncpy(prev, f[0], sizeof(prev) - 1);
            prev[sizeof(prev) - 1] = '\0';
            if (findrepo(f[0]) < 0)
                printf("%-14s %-12s %6s  [installed %s%s]\n", f[0], "-", "-", f[1],
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
        rc = fetchall(0);
        if (rc == 0 && usesnightly())
            rc = fetchall(1);
        if (rc == 0)
            pending();
        return rc;
    }
    if (!strcmp(cmd, "list"))
        return list(nightly);
    if (!strcmp(cmd, "install"))
        return install(argc - 2, argv + 2, nightly);
    if (!strcmp(cmd, "upgrade"))
        return upgrade(argc > 2 && !strcmp(argv[2], "-y"));
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
