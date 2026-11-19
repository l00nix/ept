# ept - ELKS Package Tool

A small package manager for [ELKS](https://github.com/ghaerr/elks). It downloads programs from a plain HTTP repository, verifies them, and keeps them up to date.

There are two builds, and both install as `/bin/ept`: the full ept, and [tiny ept](#usage-tiny-ept) for floppy systems. They share the same package lists and installed list, so you can switch between them at any time.

### Usage: full ept
```
ept list [-n]                 show available and installed packages
ept install [-n] [-r] <pkgs>  install or update packages
ept install -a                update everything installed
ept remove <pkgs>             remove packages
  -n  use the nightly builds instead of the tagged releases
  -r  fetch every file of the package again
```

`list` and `install` fetch the package list first, so they never work from a stale one. If a repository cannot be reached, `list` shows the list saved from last time.

`install` shows how much it will download and how many files, next to the free space and free files (inodes) on the root filesystem, and asks `[Y/n]` once for all the packages named. Scripts answer by piping: `echo | ept install <pkg>`.

A new version of a package only downloads the files that changed, and a package with nothing to download is reported as up to date without asking. `ept` goes by its record of what it installed, so a file that was damaged or replaced afterwards is not noticed; `install -r` fetches every file of the package again.

`install -a` updates everything installed. It fetches the package lists (the nightly ones too, if anything nightly is installed), shows what would change, and asks once:
```
  fetch86        1.0 -> 1.1  10K
  cowsay         20260101-0000000 -> 20261116-fd42a87  0K (nightly)
Install 10K, 1 files (396K, 11 files free)? [Y/n]
```

`-n` uses the nightly builds instead of the tagged releases. A package installed with `-n` stays on the nightly channel for `install -a` until it is installed again without `-n`.

While a file downloads, a bar fills with one `#` per sixteenth of it. Files a new version no longer has are deleted. A file belongs to the package that installed it last, so removing one package never deletes a file another package has taken over.

### Usage: tiny ept
```
ept list [-n]            show available and installed packages
ept install [-n] <pkgs>  install or update packages
ept remove <pkgs>        remove packages
```

Tiny ept is the full ept cut down to fit a floppy. It is under 7,168 bytes, so it takes 7 blocks on a MINIX floppy.

- `ept install -n tinyept` replaces the full ept with tiny ept (tiny ept is only built as a nightly for now). Run the same command once more with tiny ept to tidy the installed list: it then records `/bin/ept` as tinyept's instead of ept's.
- `ept install ept` brings the full ept back.

It works like the full ept, except:

- It only uses `http://ept-repo.com`; `/etc/ept.cfg` is not read.
- There is no `install -a`. To update everything, run `ept list` and `ept install` the packages that show `[installed <older version>]`.
- There is no `-r`, because `install` downloads every file of the package every time, including files that did not change.
- A failed download is tried again right away instead of after 5 seconds; with one repository, that is enough.

### On ELKS

- Build: `make eptdev` for the full ept or `make tiny/tinyept` for tiny ept, then copy the result to `/bin/ept`. Both need `ia16-elf-gcc` with the ELKS libc, including its regparmcall variant (`-mregparmcall`), which the ELKS libc build makes. Plain `make` still builds the 1.x `ept` from `ept.c`.
- Repository: defaults to `http://ept-repo.com` (`http://ept.samwarr.dev` before 1.10, still served). To use others, put `REPO=http://host[:port][/path]` lines in `/etc/ept.cfg`, where host is a name (looked up with DNS) or an IP address; see [Repositories](#repositories).
- State: `/etc/ept/index` and `/etc/ept/nindex` are the package lists (`index.1`, `nindex.1` and so on for extra repositories), `/etc/ept/inst` records what is installed.
- Networking must be up first (`net start`).

Every download is checked against the size and MD5 in the package list before it replaces anything. **ELKS has no TLS, so this catches corruption, not tampering.** A download that stalls for 10 seconds is tried again, up to 3 tries in all, starting the file over each time; the full ept waits 5 seconds between tries, so that ELKS networking has time to free the connections of the failed ones. If an install stops partway, the files fetched so far stay in place; run the same `ept install` again to finish it.

### Repositories

Every `REPO=` line in `/etc/ept.cfg` is a repository, up to 8 of them; if the file does not exist, `ept` creates it with the contents of [`ept.cfg`](ept.cfg) the first time it runs a command (`ept` on its own only prints the usage). Without a `REPO=` line in it, `http://ept-repo.com` is used.

**Every repository you list can install and replace files anywhere on the system, so only add ones you trust.**

- The first repository is the primary. Only it can update `ept` itself: the `ept` package and the file `/bin/ept` are never taken from another one.
- The others are extras, for things like a maintainer's test builds. For any other package a later line overrides an earlier one, so add extras below the primary.
- Packages are matched by name only. A build published under the same name as a package of another repository replaces it; to have it installed next to the original, give it its own name and its own file paths, e.g. `lua-test` in `/bin/lua-test`.
- Where a package comes from is not remembered. Add a repository that has a different build of something installed and `ept install -a` offers that build; take the line out again and it offers the previous one.
- A repository that cannot be reached, or has no nightly builds when `-n` is used, is skipped for that run.

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
eptrepo html <repo>
```

If `<file>` is a directory, everything in it is added below `<dest>`, so a whole tree is one pair, e.g. `eptrepo add repo c86 1.0 target /`. Recipes take the same form in `FILES`, e.g. `include:/usr/include`. A symlink is published as a copy of what it points to. The target is looked up inside the package, as it will be on ELKS: a link to `/lib/x` gets the package's `/lib/x`, never a file of the build host.

A package is refused, and the old one left as it was, if it has files that cannot be published: symlinks to something that is not in the package, or names with characters other than letters, digits and `. _ + -`. With `-s` (`SKIP=yes` in a recipe) those files are reported and skipped instead.

`-d` (`DESC="..."` in a recipe) sets the description that `ept search` shows; keep it under 65 characters so it fits on a line. Without `-d` a package keeps the description it had.

`eptrepo index` builds the list from whatever is in `pool/`, so a package can also be maintained by hand: copy the files to `pool/<name>/`, write `pool/<name>/VERSION` (and `DESC` if you like), and run `eptrepo index <repo>`.

`eptrepo html` writes `<repo>/index.html`, the home page a browser gets at `/`: how to get started, the commands, and every package with its release and nightly versions. It is plain HTML that old browsers like ELKSWeb can show. The builder rewrites it after every build cycle; set `EPT_URL` to the address people should use for the repository (default `http://ept-repo.com`).

### License

MIT, see [LICENSE](LICENSE). `md5.c` is public domain.
