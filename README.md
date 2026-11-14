# ept - ELKS Package Tool

A small package manager for [ELKS](https://github.com/ghaerr/elks). It downloads programs from a plain HTTP repository, verifies them, and keeps them up to date.

### Usage
```
ept update                  fetch the package lists
ept list [-n]               show available and installed packages
ept install [-n] <pkg> ...  install or reinstall packages
ept upgrade                 update all installed packages
ept remove <pkg> ...        remove packages
```

`-n` uses the nightly builds instead of the tagged releases. A package installed with `-n` stays on the nightly channel for `ept upgrade` until it is installed again without `-n`.

### On ELKS

- Build: `make` (needs `ia16-elf-gcc` with the ELKS libc), then copy `ept` to `/bin/ept`.
- Repository: defaults to `http://ept.samwarr.dev`. To use another one, put `REPO=http://host[:port][/path]` in `/etc/ept.cfg`.
- State: `/etc/ept/index` and `/etc/ept/nindex` are the package lists, `/etc/ept/inst` records what is installed.
- Networking must be up first (`net start`).

Every download is checked against the size and MD5 in the package list before it replaces anything. **ELKS has no TLS, so this catches corruption, not tampering.**

### Repository format

```
<repo>/index                    package list for tagged releases
<repo>/pool/<name>/VERSION      version, one word
<repo>/pool/<name>/<dest>       each file at its install path, e.g. pool/lua/bin/lua
<repo>/nightly/...              same layout again for nightly builds
```

A project without tags only gets nightlies.

Each index line is `name version md5 size mode dest`; the file starts with `#ept1` and ends with `#end`. `server/eptrepo` maintains it:

```
eptrepo add <repo> <name> <version> <file> <dest> [<file> <dest> ...]
eptrepo index <repo>
```

