#!/bin/bash
# ept-build - build packages from git and publish them to the ept repository
#
#   ept-build loop          build everything, then repeat every $INTERVAL seconds
#   ept-build once          build everything once
#   ept-build once <name>   build one recipe, even if nothing changed
#
# Each /recipes/<name>.pkg is a shell fragment that sets:
#   GIT     git URL of the project
#   BUILD   command that builds it, run in the checkout
#   FILES   "built-file:/install/path ..." pairs; a directory on the left
#           installs everything in it below the path, e.g. "include:/usr/include"
#   BRANCH  (optional) branch nightlies are built from, default: remote HEAD
#   STABLE  (optional) set to "no" to publish nightlies only and ignore tags
#
# Recipes are read from /recipes, or, when RECIPES_GIT is set, from that git
# repository (subdirectory RECIPES_SUBDIR), which is pulled before every cycle.
#
# Tagged releases go to /repo, nightlies to /repo/nightly:
#   stable version  = newest git tag, without a leading v
#   nightly version = <commit date>-<short hash> of the branch head

REPO=${REPO:-/repo}
WORK=${WORK:-/work}
RECIPES=${RECIPES:-/recipes}
RECIPES_GIT=${RECIPES_GIT:-}
RECIPES_SUBDIR=${RECIPES_SUBDIR:-}
ELKS=${ELKS:-$WORK/elks}
ELKS_GIT=${ELKS_GIT:-https://github.com/ghaerr/elks.git}
INTERVAL=${INTERVAL:-86400}
export ELKS
export PATH=$ELKS/elks/tools/bin:$PATH		# elf2elks, elks-compress

log() { echo "[$(date '+%F %T')] $*"; }

# Clone or update ELKS and rebuild its libc when the tree changed
update_elks()
{
	local old new
	if [ ! -d "$ELKS/.git" ]; then
		log "cloning ELKS"
		git clone -q --depth 1 "$ELKS_GIT" "$ELKS" || return 1
	else
		old=$(git -C "$ELKS" rev-parse HEAD)
		git -C "$ELKS" pull -q --ff-only || log "warning: ELKS pull failed, using existing tree"
	fi
	new=$(git -C "$ELKS" rev-parse HEAD)
	export TOPDIR=$ELKS CROSSDIR=$ELKS/cross
	if [ "$old" != "$new" ] || [ ! -f "$ELKS/cross/ia16-elf/lib/elkslibc/libc.a" ]; then
		log "building ELKS libc at $(git -C "$ELKS" rev-parse --short HEAD)"
		(
			cd "$ELKS" || exit 1
			[ -f .config ] || cp ibmpc-1440.config .config
			make -s include/autoconf.h < /dev/null &&
			make -s -C elks/tools all &&	# elf2elks, needed to link
			make -s libc
		) > "$WORK/elks-libc.log" 2>&1 || {
			log "ERROR: ELKS libc build failed, see $WORK/elks-libc.log"
			tail -20 "$WORK/elks-libc.log" | cut -c1-200
			return 1
		}
	fi
	# Open Watcom large-model libc (libcl.lib), for packages built with owcc.
	# A failure here only affects those packages, so it is not fatal.
	if [ -x "$WATCOM/binl64/owcc" ] && { [ "$old" != "$new" ] || [ ! -f "$ELKS/libc/libcl.lib" ]; }; then
		log "building ELKS libc for Open Watcom"
		(cd "$ELKS/libc" && PATH=$WATCOM/binl64:$PATH make -s -f watcom.mk MODEL=l) \
			> "$WORK/elks-owlibc.log" 2>&1 || {
			log "ERROR: Open Watcom libc build failed, see $WORK/elks-owlibc.log"
			tail -20 "$WORK/elks-owlibc.log" | cut -c1-200
		}
	fi
	# Replace the libc bundled with the toolchain package by the one just
	# built, so -melks-libc always means the current ELKS libc. /usr is not
	# a volume, so this is redone whenever the container is recreated.
	if [ "$(cat /usr/ia16-elf/lib/elkslibc/.ept-rev 2>/dev/null)" != "$new" ]; then
		make -s -C "$ELKS/libc" DESTDIR=/usr install >> "$WORK/elks-libc.log" 2>&1 || {
			log "ERROR: ELKS libc install failed, see $WORK/elks-libc.log"
			return 1
		}
		echo "$new" > /usr/ia16-elf/lib/elkslibc/.ept-rev
		log "ELKS libc $(git -C "$ELKS" rev-parse --short HEAD) installed"
	fi
}

# With RECIPES_GIT set, clone or update that repository and read recipes from it.
# If the pull fails, the last copy that was fetched is used.
update_recipes()
{
	local dir=$WORK/recipes
	[ -n "$RECIPES_GIT" ] || return 0
	if [ ! -d "$dir/.git" ]; then
		git clone -q "$RECIPES_GIT" "$dir" || { log "ERROR: cannot clone recipes from $RECIPES_GIT"; return 1; }
	elif ! { git -C "$dir" fetch -q origin && git -C "$dir" reset -q --hard '@{upstream}'; }; then
		log "warning: recipes update failed, using the existing copy"
	fi
	RECIPES=$dir${RECIPES_SUBDIR:+/$RECIPES_SUBDIR}
	[ -d "$RECIPES" ] || { log "ERROR: $RECIPES_SUBDIR not found in the recipes repository"; return 1; }
	log "recipes at $(git -C "$dir" rev-parse --short HEAD): $(ls "$RECIPES" | grep -c '\.pkg$') packages"
}

# build_one <name> <git ref> <version> <repo dir>
build_one()
{
	local name=$1 ref=$2 version=$3 repo=$4 src=$WORK/src/$1 args=() pair
	log "$name: building $version from $ref"
	git -C "$src" checkout -q -f "$ref" && git -C "$src" clean -q -fdx || return 1
	if ! (cd "$src" && eval "$BUILD") > "$WORK/log/$name.log" 2>&1; then
		log "ERROR: $name $version build failed, see $WORK/log/$name.log"
		tail -15 "$WORK/log/$name.log" | cut -c1-200
		return 1
	fi
	for pair in $FILES; do
		if [ -d "$src/${pair%%:*}" ]; then
			if [ -z "$(find -L "$src/${pair%%:*}" -type f | head -1)" ]; then
				log "ERROR: $name: ${pair%%:*} contains no files"
				return 1
			fi
		elif [ ! -s "$src/${pair%%:*}" ]; then
			log "ERROR: $name: build did not produce ${pair%%:*}"
			return 1
		fi
		args+=("$src/${pair%%:*}" "${pair#*:}")
	done
	mkdir -p "$repo"
	eptrepo add "$repo" "$name" "$version" "${args[@]}" && log "$name $version published to $repo"
}

# build_recipe <file> [force]
build_recipe()
{
	local recipe=$1 force=$2 name src tag head version
	name=$(basename "$recipe" .pkg)
	GIT= BUILD= FILES= BRANCH= STABLE=
	. "$recipe"
	if [ -z "$GIT" ] || [ -z "$BUILD" ] || [ -z "$FILES" ]; then
		log "ERROR: $recipe must set GIT, BUILD and FILES"
		return 1
	fi
	src=$WORK/src/$name
	if [ ! -d "$src/.git" ]; then
		git clone -q "$GIT" "$src" || { log "ERROR: $name: cannot clone $GIT"; return 1; }
	else
		git -C "$src" fetch -q --tags --prune origin || { log "ERROR: $name: fetch failed"; return 1; }
	fi

	# stable: newest tag (versionsort.suffix makes v0.3 newer than v0.3-dev1)
	tag=$(git -C "$src" -c versionsort.suffix=- tag --sort=-v:refname | head -1)
	if [ "$STABLE" = no ]; then
		:
	elif [ -n "$tag" ]; then
		version=${tag#v}
		if [ -n "$force" ] || [ "$(cat "$REPO/pool/$name/VERSION" 2>/dev/null)" != "$version" ]; then
			build_one "$name" "$tag" "$version" "$REPO"
		fi
	else
		log "$name: no tags yet, only publishing nightlies"
	fi

	# nightly: head of the branch
	head=origin/${BRANCH:-HEAD}
	version=$(git -C "$src" log -1 --date=format:%Y%m%d --format=%cd-%h "$head") || return 1
	if [ -n "$force" ] || [ "$(cat "$REPO/nightly/pool/$name/VERSION" 2>/dev/null)" != "$version" ]; then
		build_one "$name" "$head" "$version" "$REPO/nightly"
	fi
}

build_all()
{
	local recipe
	mkdir -p "$WORK/src" "$WORK/log" "$REPO/pool" "$REPO/nightly/pool"
	update_elks || return 1
	update_recipes || return 1
	if [ -n "$1" ]; then
		build_recipe "$RECIPES/$1.pkg" force
	else
		for recipe in "$RECIPES"/*.pkg; do
			[ -f "$recipe" ] && build_recipe "$recipe"
		done
	fi
	# keep both indexes valid even when there are no packages yet
	eptrepo index "$REPO" > /dev/null
	eptrepo index "$REPO/nightly" > /dev/null
}

# Run one build cycle while holding $WORK/build.lock. The container's own
# "loop" and a manual "exec ... once" would otherwise run at the same time,
# racing on git checkouts and on the repository files.
locked()
{
	mkdir -p "$WORK"
	(
		flock -n 9 || { log "another build is running, waiting for it"; flock 9; }
		"$@"
	) 9> "$WORK/build.lock"
}

case "$1" in
once)	locked build_all "$2";;
loop)	while :; do
		locked build_all
		log "sleeping $INTERVAL seconds"
		sleep "$INTERVAL"
	done;;
*)	sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 1;;
esac
