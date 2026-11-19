/*
 * tiny ept - a small ept for ELKS floppy images
 *
 * Copyright (c) 2026 Sam Warr
 * SPDX-License-Identifier: MIT
 *
 *   ept list [-n]             what the repository has, and what is installed
 *   ept install [-n] <pkg> ...    install or update packages
 *   ept remove <pkg> ...      remove packages
 *   -n   the nightly builds, as for the full ept
 *
 * install first shows the space and files it needs against what is free on
 * the root filesystem, and asks. Scripts answer it: echo | ept install <pkg>
 *
 * "ept install ept" replaces this with the full ept, which carries on with the
 * same files. Only the basics are here: the repository is built in and looked
 * up with DNS alone (no /etc/ept.cfg, no /etc/hosts, no stdio). A download gets
 * TRIES tries, each given up after STALL seconds without data.
 *
 * All it relies on is the repository format, which every ept shares:
 *   <repo>/index         first line "#ept1", last line "#end", other "#" lines
 *                        ignored; one line per file: name version md5 size mode dest
 *   <repo>/pool/<name><dest>   each file
 *   <repo>/nightly/...   the same again for the nightly builds
 * and the installed list /etc/ept/inst: name version md5 dest channel (s or n)
 */
#include <stdint.h>
#include <stdio.h>              /* rename() only; no stdio is used */
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef REPOHOST                /* overridable for testing */
#define REPOHOST    "ept-repo.com"
#define REPOPORT    80
#endif
#ifndef ROOT
#define ROOT        ""          /* prefix for every local path, for testing */
#endif
#define STALL       10          /* seconds without data before a try is given up */
#define TRIES       3           /* tries per download */
#define DIR         ROOT "/etc/ept"
#define INDEX       DIR "/index"
#define NINDEX      DIR "/nindex"
#define DB          DIR "/inst"
#define DBNEW       DIR "/inst.new"

#ifdef OLD_RESOLV               /* ELKS libc before in_resolv() took ancount */
#define resolve(h)  in_resolv(h, NULL)
#else
#define resolve(h)  in_resolv(h, NULL, NULL)
#endif

static char buf[1024];          /* network and copying */
static char fbuf[512];          /* reading the lists */
static char line[256];          /* the line being handed out; longer ones are cut */
static int llen;
static void (*online)(char *);
static char path[192], tmp[192];
static char *pkg;               /* the package being installed or removed */
static int found, bad, drop, outfd;
static const char *why;         /* why the last try failed */
static int nfiles;              /* files of the packages to install */
static char newf[512];          /* files of the new version, NUL separated */
static unsigned nnew;           /* their length; more than fits: delete nothing */
static char rpath[200];         /* the file being downloaded */
static ipaddr_t ip;             /* the repository's address, once looked up */
static char *chan = "";         /* -n: "/nightly" */
static char *idx = INDEX;       /* the list of the channel */
static char cname[] = " s\n";  /* the channel in the installed list */
static char *f[6];              /* fields of the current line */

/* ---- MD5 (RFC 1321), one step per pass; x86 is little-endian, so the
 * block and the digest are used in place */
static uint32_t st[4];
static union { unsigned char c[64]; uint32_t w[16]; } mb;
static unsigned long mlen;

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const unsigned char S[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void md5_block(void)
{
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], x;
    int i, g, s;

    for (i = 0; i < 64; i++) {
        switch (i >> 4) {
        case 0:  x = (b & c) | (~b & d); g = i;                break;
        case 1:  x = (d & b) | (~d & c); g = (5 * i + 1) & 15; break;
        case 2:  x = b ^ c ^ d;          g = (3 * i + 5) & 15; break;
        default: x = c ^ (b | ~d);       g = (7 * i) & 15;     break;
        }
        x += a + K[i] + mb.w[g];
        s = S[(i >> 4) << 2 | (i & 3)];
        a = d;
        d = c;
        c = b;
        b += x << s | x >> (32 - s);
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

static void md5_init(void)
{
    st[0] = 0x67452301;
    st[1] = 0xefcdab89;
    st[2] = 0x98badcfe;
    st[3] = 0x10325476;
    mlen = 0;
}

static void md5_add(const char *p, int n)
{
    while (n--) {
        mb.c[(unsigned)mlen & 63] = *p++;
        if (!((unsigned)++mlen & 63))
            md5_block();
    }
}

/* finish, then compare with a hex digest: 0 if they match */
static int md5_check(const char *sum)
{
    static const char hex[] = "0123456789abcdef";
    unsigned long n = mlen;
    unsigned char *d = (unsigned char *)st;
    int i;

    md5_add("\200", 1);
    while (((unsigned)mlen & 63) != 56)
        md5_add("", 1);
    mb.w[14] = n << 3;
    mb.w[15] = n >> 29;
    md5_block();
    for (i = 0; i < 16; i++)
        if (sum[2 * i] != hex[d[i] >> 4] || sum[2 * i + 1] != hex[d[i] & 15])
            return 1;
    return 0;
}

/* ---- output, without stdio */
static void out(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

static void say(const char *a, const char *b)
{
    write(STDERR_FILENO, "ept: ", 5);
    write(STDERR_FILENO, a, strlen(a));
    write(STDERR_FILENO, b, strlen(b));
    write(STDERR_FILENO, "\n", 1);
}

/* n in decimal; 16 bits is enough for K and file counts */
static char *num(unsigned n)
{
    static char t[6];
    char *p = t + 5;

    do
        *--p = '0' + n % 10;
    while (n /= 10);
    return p;
}

static unsigned long atoun(const char *s, int base)
{
    unsigned long n = 0;

    while (*s >= '0' && *s < '0' + base)
        n = n * base + (*s++ - '0');
    return n;
}

/* ---- reading lists line by line */
static void feed(const char *p, int n)
{
    char c;

    while (n--) {
        if ((c = *p++) == '\n') {
            if (llen && line[llen - 1] == '\r')
                llen--;
            line[llen] = '\0';
            online(line);
            llen = 0;
        } else if (llen < sizeof(line) - 1)
            line[llen++] = c;
    }
}

/* call fn for each line of a file; -1 if it cannot be read */
static int eachline(const char *file, void (*fn)(char *))
{
    int fd, n;

    if ((fd = open(file, O_RDONLY)) < 0)
        return -1;
    online = fn;
    llen = 0;
    while ((n = read(fd, fbuf, sizeof(fbuf))) > 0)
        feed(fbuf, n);
    close(fd);
    if (llen)
        feed("\n", 1);
    return n < 0 ? -1 : 0;
}

/* split s at blanks into f[]; the number of fields */
static int split(char *s)
{
    int n = 0;

    while (n < 6) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        f[n++] = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        if (*s)
            *s++ = '\0';
    }
    return n;
}

/* ---- HTTP */

/* SIGALRM only has to interrupt a read() that waits too long */
static void onalarm(int sig)
{
}

/* GET <path> into fd. 0 on a complete 200 reply of size bytes (any size if
 * size < 0) whose md5 matches sum (if sum) */
static int get(const char *rpath, int fd, long size, const char *sum)
{
    int s, n, i, hdr = 1, nl = 0, slen = 0, rc = -1;
    unsigned long total = 0;
    char status[12];
    struct sockaddr_in a;

    why = "cannot reach " REPOHOST;
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    a.sin_family = AF_INET;
    a.sin_port = PORT_ANY;
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
        goto done;
    a.sin_port = htons(REPOPORT);
    /* look the host up once: ELKS asks DNS over TCP, and ktcp has room for
     * only a few connections at a time, closed ones included for a while */
    if (!ip)
        ip = resolve(REPOHOST);
    if (!(a.sin_addr.s_addr = ip)
            || in_connect(s, (struct sockaddr *)&a, sizeof(a), 10) < 0)
        goto done;
    why = "connection lost";
    strcpy(buf, "GET ");
    strcat(buf, rpath);
    strcat(buf, " HTTP/1.0\r\nHost: " REPOHOST "\r\nUser-Agent: ept-tiny\r\nConnection: close\r\n\r\n");
    n = strlen(buf);
    if (write(s, buf, n) != n)
        goto done;
    md5_init();
    signal(SIGALRM, onalarm);
    while (alarm(STALL), (n = read(s, buf, sizeof(buf))) > 0) {
        for (i = 0; i < n && hdr; i++) {
            if (slen < sizeof(status))
                status[slen++] = buf[i];
            if (buf[i] == '\n') {
                if (nl)
                    hdr = 0;            /* blank line ends the header */
                nl = 1;
            } else if (buf[i] != '\r')
                nl = 0;
        }
        if (hdr)
            continue;
        if (slen < sizeof(status) || strncmp(status, "HTTP/1.", 7) || strncmp(status + 9, "200", 3)) {
            why = "server error";
            goto done;
        }
        n -= i;
        if (write(fd, buf + i, n) != n) {
            why = "write error";
            goto done;
        }
        md5_add(buf + i, n);
        if (sum && (total ^ (total + n)) >> 12)
            out(".");                   /* a heartbeat per 4K */
        total += n;
    }
    if (n >= 0 && !hdr && (size < 0 || total == size)) {
        why = "checksum mismatch";
        if (!sum || !md5_check(sum))
            rc = 0;
    }
done:
    alarm(0);
    close(s);
    return rc;
}

/* rename, or copy where that fails (FAT); the old file must already be gone */
static int put(const char *from, const char *to)
{
    int in, o, n;

    if (rename(from, to) == 0)
        return 0;
    if ((in = open(from, O_RDONLY)) < 0)
        return -1;
    if ((o = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0 && write(o, buf, n) == n)
        ;
    close(in);
    if (close(o) < 0 || n != 0)
        return -1;
    unlink(from);
    return 0;
}

/* replace file 'to' by 'from'; a running program can be unlinked but not overwritten */
static int replace(const char *from, const char *to)
{
    unlink(to);
    if (put(from, to) != 0) {
        say("cannot write ", to);
        unlink(from);
        return -1;
    }
    return 0;
}

/* download <path> to file, up to TRIES times; only the last failure says why */
static int fetch(const char *rpath, const char *file, long size, const char *sum)
{
    int fd, rc, try;

    for (try = 1; ; try++) {
        if ((fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
            say("cannot write ", file);
            return -1;
        }
        rc = get(rpath, fd, size, sum);
        if (close(fd) < 0) {
            rc = -1;
            why = "write error";
        }
        if (rc == 0)
            return 0;
        if (try == TRIES)
            break;
        say("retrying", "");
    }
    say(why, "");
    unlink(file);
    return -1;
}

/* ---- the package list */
static int lines, known, last;

static void checkline(char *s)
{
    if (lines++ == 0)
        known = !strcmp(s, "#ept1");    /* a list this ept understands */
    last = !strcmp(s, "#end");
}

static int update(void)
{
    mkdir(DIR, 0755);
    strcpy(rpath, chan);
    strcat(rpath, "/index");
    if (fetch(rpath, idx, -1, NULL) != 0)
        return -1;
    lines = known = last = 0;
    eachline(idx, checkline);
    if (!known || !last) {              /* a newer format, or cut short */
        say("unusable package list", "");
        unlink(idx);
        return -1;
    }
    return 0;
}

/* a package line of the list, in f[]? */
static int pkgline(char *s)
{
    return s[0] != '#' && split(s) == 6;
}

/* ---- list */
static char inst[512];          /* installed packages: "name version\0" each */
static char prev[256], avail[256]; /* fields of a line fit */
static unsigned long total;

static char *instver(const char *name);

static void instline(char *s)
{
    if (split(s) != 5 || instver(f[0]))
        return;
    if (strlen(inst) + strlen(f[0]) + strlen(f[1]) + 3 < sizeof(inst)) {
        strcat(inst, " ");
        strcat(inst, f[0]);
        strcat(inst, " ");
        strcat(inst, f[1]);
        strcat(inst, " ");
    }
}

/* the installed version of name, or NULL */
static char *instver(const char *name)
{
    static char v[24];
    char *p, *e;
    int n = strlen(name);

    for (p = inst + 1; *p; p++)         /* " name version " */
        if (p[-1] == ' ' && !strncmp(p, name, n) && p[n] == ' ') {
            p += n + 1;
            e = strchr(p, ' ');
            if (e - p < sizeof(v)) {
                memcpy(v, p, e - p);
                v[e - p] = '\0';
                return v;
            }
        }
    return NULL;
}

static void pad(const char *s, int w)
{
    out(s);
    for (w -= strlen(s); w > 0; w--)
        out(" ");
}

static void showpkg(void)
{
    char *v;

    if (!prev[0])
        return;
    pad(prev, 15);
    pad(avail, 13);
    pad("", 5 - strlen(num((total + 1023) / 1024)));
    out(num((total + 1023) / 1024));
    out("K");
    if ((v = instver(prev)) != NULL) {
        out("  [installed");
        if (strcmp(v, avail)) {
            out(" ");
            out(v);
        }
        out("]");
    }
    out("\n");
}

static void listline(char *s)
{
    if (!pkgline(s))
        return;
    if (strcmp(f[0], prev)) {
        showpkg();
        strcpy(prev, f[0]);
        strcpy(avail, f[1]);
        total = 0;
    }
    total += atoun(f[3], 10);
}

static int list(void)
{
    inst[0] = ' ';
    eachline(DB, instline);
    eachline(idx, listline);
    showpkg();
    return 0;
}

/* ---- install */

/* is p anything but an absolute path without empty, "." or ".." parts? */
static int badpath(const char *p)
{
    const char *q;
    int n;

    if (*p != '/' || strlen(ROOT) + strlen(p) + 8 >= sizeof(path))
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

/* download and install one file of the package */
static void fileline(char *s)
{
    char *p;
    unsigned n;

    if (bad || !pkgline(s) || strcmp(f[0], pkg))
        return;
    if (!found++) {
        out("Installing ");
        out(pkg);
        out(" ");
        out(f[1]);
        out("\n");
    }
    if (badpath(f[5])) {
        say("bad path in package list: ", f[5]);
        bad = 1;
        return;
    }
    n = strlen(f[5]) + 1;
    if (nnew + n <= sizeof(newf))
        memcpy(newf + nnew, f[5], n);
    nnew += n;
    out("  ");
    out(f[5]);
    out(" ");
    strcpy(path, ROOT);
    strcat(path, f[5]);
    for (p = path + strlen(ROOT) + 1; *p; p++)  /* create the parent directories */
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
        }
    strcpy(tmp, path);
    strcpy(strrchr(tmp, '/') + 1, "ept.tmp");
    strcpy(rpath, chan);
    strcat(rpath, "/pool/");
    strcat(rpath, pkg);
    strcat(rpath, f[5]);
    if (fetch(rpath, tmp, atoun(f[3], 10), f[2]) != 0
            || replace(tmp, path) != 0) {
        bad = 1;
        return;
    }
    chmod(path, atoun(f[4], 8));
    out("\n");
}

/* is d one of the files just installed? (only if they all fitted in newf) */
static int innew(const char *d)
{
    char *p;

    for (p = newf; p < newf + nnew; p += strlen(p) + 1)
        if (!strcmp(p, d))
            return 1;
    return 0;
}

/* the installed list without the package. Its files are deleted when it is
 * being removed, or when the new version no longer has them. A file that
 * another package listed now belongs to the package just installed */
static void keepline(char *s)
{
    char *t = s + strlen(s);

    if (split(s) != 5)
        return;
    if (!strcmp(f[0], pkg)) {
        if (!drop && (nnew > sizeof(newf) || innew(f[3])))
            return;                     /* the new version has it too, or can't tell */
        strcpy(path, ROOT);
        strcat(path, f[3]);
        unlink(path);
    } else if (!drop && nnew <= sizeof(newf) && innew(f[3])) {
        return;                         /* it belongs to the new package now */
    } else {
        for (; s < t; s++)              /* split() put NULs between the fields */
            if (!*s)
                *s = ' ';
        *t++ = '\n';
        write(outfd, line, t - line);
    }
}

/* the package's lines from the package list, as installed lines */
static void newline(char *s)
{
    if (!pkgline(s) || strcmp(f[0], pkg))
        return;
    strcpy(buf, f[0]);
    strcat(buf, " ");
    strcat(buf, f[1]);
    strcat(buf, " ");
    strcat(buf, f[2]);
    strcat(buf, " ");
    strcat(buf, f[5]);
    strcat(buf, cname);
    write(outfd, buf, strlen(buf));
}

/* write the installed list without pkg, plus pkg's lines from the package list if add */
static int rewrite(int add)
{
    drop = !add;
    if ((outfd = open(DBNEW, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        say("cannot write ", DBNEW);
        return -1;
    }
    eachline(DB, keepline);
    if (add)
        eachline(idx, newline);
    if (close(outfd) < 0)
        return -1;
    return replace(DBNEW, DB);
}

/* add up the size and files of the package */
static void sizeline(char *s)
{
    if (pkgline(s) && !strcmp(f[0], pkg)) {
        total += atoun(f[3], 10);
        nfiles++;
    }
}

/* show what the install needs and what is free; 1 to go ahead */
static int ask(void)
{
    struct stat st;
    struct statfs sf;

    out("Install ");
    out(num((total + 1023) / 1024));
    out("K, ");
    out(num(nfiles));
    out(" files");
    if (stat(ROOT "/", &st) == 0 && ustatfs(st.st_dev, &sf, 0) == 0) {
        out(" (");
        out(num(sf.f_bavail > 65535 ? 65535 : sf.f_bavail));
        out("K");
        if (sf.f_files) {               /* MINIX; FAT has no limit on files */
            out(", ");
            out(num(sf.f_ffree));
            out(" files");
        }
        out(" free)");
    }
    out("? [Y/n] ");
    return read(0, buf, sizeof(buf)) > 0 && (buf[0] == 'y' || buf[0] == 'Y' || buf[0] == '\n');
}

static int install(char *name)
{
    pkg = name;
    found = bad = nnew = 0;
    eachline(idx, fileline);
    if (!found) {
        say("no package named ", name);
        return 1;
    }
    if (bad)
        return 1;
    if (rewrite(1) != 0)
        return 1;
    if (!strcmp(name, "ept"))
        out("The full ept is installed; run ept again to use it\n");
    return 0;
}

static int remove_(char *name)
{
    pkg = name;
    inst[0] = ' ';
    inst[1] = '\0';
    eachline(DB, instline);
    if (!instver(name)) {
        say(name, " is not installed");
        return 1;
    }
    out("Removing ");
    out(name);
    out("\n");
    return rewrite(0) != 0;
}

int main(int argc, char **argv)
{
    int i, rc = 0;

    if (argc > 2 && !strcmp(argv[2], "-n")) {
        chan = "/nightly";
        idx = NINDEX;
        cname[1] = 'n';
        argv[2] = argv[1];
        argv++;
        argc--;
    }
    if (argc < 2 || (strcmp(argv[1], "list") && argc < 3)
            || (strcmp(argv[1], "list") && strcmp(argv[1], "install") && strcmp(argv[1], "remove"))) {
        out("usage: ept list [-n] | ept install [-n] <pkgs> | ept remove <pkgs>\n"
            "This is tinyept; \"ept install ept\" installs the full one.\n");
        return 1;
    }
    if (argv[1][0] == 'r') {
        for (i = 2; i < argc; i++)
            rc |= remove_(argv[i]);
        return rc;
    }
    out("Fetching http://" REPOHOST);
    out(chan);
    out("/index\n");
    if (update() != 0)
        return 1;
    if (argv[1][0] == 'l')
        return list();
    for (i = 2; i < argc; i++) {        /* one question for all the packages */
        pkg = argv[i];
        eachline(idx, sizeline);
    }
    if (total && !ask())
        return 0;
    for (i = 2; i < argc; i++)
        rc |= install(argv[i]);
    return rc;
}
