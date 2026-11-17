# ept - ELKS Package Tool

A small package manager for [ELKS](https://github.com/ghaerr/elks). It downloads programs from a plain HTTP repository, verifies them, and keeps them up to date.

### Usage
```
ept update                       fetch the package lists
ept list [-n]                    show available and installed packages
ept search [-n] [<text>]         find packages by name or description
ept install [-n] [-r] <pkg> ...  install or repair packages
ept upgrade [-y]                 update all installed packages
ept remove <pkg> ...             remove packages
```

`list`, `install` and `upgrade` fetch the package list first, so they never work from a stale one.

`search` shows each package with its one line description, or only those with `<text>` in the name or description, in any case. Descriptions are fetched for `search` alone; a repository that has none just shows the names.

`install` shows how many files and how much it has to download for each package and asks first, and `upgrade` does the same with what would change; `-y` skips the question. Nothing is asked when there is nothing to download, or for `install -r`.

A new version of a package only downloads the files that changed. `ept` goes by its record of what it installed, so a file that was damaged or replaced afterwards is not noticed. `install -r` repairs that: it checks the size and MD5 of every file of the package on disk and downloads the ones that differ or are missing.

`-n` uses the nightly builds instead of the tagged releases. A package installed with `-n` stays on the nightly channel for `ept upgrade` until it is installed again without `-n`.

### On ELKS

- Build: `make` (needs `ia16-elf-gcc` with the ELKS libc), then copy `ept` to `/bin/ept`.
- Repository: defaults to `http://ept.samwarr.dev`. To use others, put `REPO=http://host[:port][/path]` lines in `/etc/ept.cfg`; see [Repositories](#repositories).
- State: `/etc/ept/index` and `/etc/ept/nindex` are the package lists (`index.1`, `nindex.1` and so on for extra repositories), `/etc/ept/inst` records what is installed.
- Networking must be up first (`net start`).

Every download is checked against the size and MD5 in the package list before it replaces anything. **ELKS has no TLS, so this catches corruption, not tampering.** If an install stops partway, the files fetched so far stay in place; run the same `ept install` again to finish it.

### Repositories

Every `REPO=` line in `/etc/ept.cfg` is a repository, up to 8 of them; if the file does not exist, `ept` creates it with the contents of [`ept.cfg`](ept.cfg) the first time it runs a command (`ept` on its own only prints the usage). Without a `REPO=` line in it, `http://ept.samwarr.dev` is used.

**Every repository you list can install and replace files anywhere on the system, so only add ones you trust.**

- The first repository is the primary. Only it can update `ept` itself: the `ept` package and the file `/bin/ept` are never taken from another one.
- The others are extras, for things like a maintainer's test builds. For any other package a later line overrides an earlier one, so add extras below the primary.
- Packages are matched by name only. A build published under the same name as a package of another repository replaces it; to have it installed next to the original, give it its own name and its own file paths, e.g. `lua-test` in `/bin/lua-test`.
- Where a package comes from is not remembered. Add a repository that has a different build of something installed and `ept upgrade` offers that build; take the line out again and it offers the previous one.
- A repository that cannot be reached, or has no nightly builds when `-n` is used, is skipped for that run. So is an extra that needs a newer `ept` than the primary provides.

### Repository format

```
<repo>/index                    package list for tagged releases
<repo>/desc                     package descriptions, one "name text" line each
<repo>/pool/<name>/VERSION      version, one word
<repo>/pool/<name>/DESC         description, one line (optional)
<repo>/pool/<name>/<dest>       each file at its install path, e.g. pool/lua/bin/lua
<repo>/nightly/...              same layout again for nightly builds
```

A project without tags only gets nightlies.

Each index line is `name version md5 size mode dest`; the file starts with `#ept1` and ends with `#end`. A `#min <version>` line names the oldest `ept` that can use the repository: an older one installs the `ept` package and starts again before it installs anything else. The version comes from `MINEPT` in `server/eptrepo`. `server/eptrepo` maintains it:

```
eptrepo add [-s] [-d <text>] <repo> <name> <version> <file> <dest> [<file> <dest> ...]
eptrepo index <repo>
```

If `<file>` is a directory, everything in it is added below `<dest>`, so a whole tree is one pair, e.g. `eptrepo add repo c86 1.0 target /`. Recipes take the same form in `FILES`, e.g. `include:/usr/include`. A symlink is published as a copy of what it points to. The target is looked up inside the package, as it will be on ELKS: a link to `/lib/x` gets the package's `/lib/x`, never a file of the build host.

A package is refused, and the old one left as it was, if it has files that cannot be published: symlinks to something that is not in the package, or names with characters other than letters, digits and `. _ + -`. With `-s` (`SKIP=yes` in a recipe) those files are reported and skipped instead.

`-d` (`DESC="..."` in a recipe) sets the description that `ept search` shows; keep it under 65 characters so it fits on a line. Without `-d` a package keeps the description it had.

`eptrepo index` builds the list from whatever is in `pool/`, so a package can also be maintained by hand: copy the files to `pool/<name>/`, write `pool/<name>/VERSION` (and `DESC` if you like), and run `eptrepo index <repo>`.

### License

MIT, see [LICENSE](LICENSE). `md5.c` is public domain.
