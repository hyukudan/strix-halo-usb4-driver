#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Apply patches/series (and optionally patches/kernel-backports/ and the
# experimental patches/experimental/series on top) to a pristine pinned Linux
# tree. Canonical procedure used by README.md, BUILD.md and CI.
#
# usage: tools/apply-series.sh [--with-backports] [--with-experimental] KERNEL_DIR
#
# Fail fast: every patch is checked against the current tree before it is
# applied and the first failure stops with a non-zero status naming the patch.
# Nothing is applied when the series file has an unsafe or missing entry.
set -eu

backports=0
experimental=0
while [ $# -gt 1 ]; do
	case $1 in
	--with-backports) backports=1 ;;
	--with-experimental) experimental=1 ;;
	*) break ;;
	esac
	shift
done
if [ $# -ne 1 ] || [ ! -d "$1" ]; then
	echo "usage: $0 [--with-backports] [--with-experimental] KERNEL_DIR" >&2
	exit 2
fi
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=$(CDPATH= cd -- "$1" && pwd)
# Never apply relative to a surrounding repository: git apply would silently
# skip the paths if KERNEL_DIR were a plain directory inside another checkout.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
GIT_CEILING_DIRECTORIES=$(dirname -- "$kernel")
export GIT_CEILING_DIRECTORIES

# Validate the whole list before touching the tree.
list=$(mktemp)
trap 'rm -f "$list"' EXIT
add_series() {
	while IFS= read -r patch || [ -n "$patch" ]; do
		case $patch in
		'' | '#'*) continue ;;
		*/* | .*) echo "apply-series: unsafe series entry: $patch" >&2; exit 1 ;;
		esac
		if [ ! -f "$1/$patch" ] || [ -L "$1/$patch" ]; then
			echo "apply-series: missing or symlinked patch: $patch" >&2
			exit 1
		fi
		printf '%s\n' "$1/$patch" >> "$list"
	done < "$1/series"
}
add_series "$repo/patches"
if [ "$backports" -eq 1 ]; then
	for patch in "$repo"/patches/kernel-backports/*.patch; do
		[ -f "$patch" ] && printf '%s\n' "$patch" >> "$list"
	done
fi
# Experimental patches are not part of the default install path.
if [ "$experimental" -eq 1 ]; then
	add_series "$repo/patches/experimental"
fi

count=0
while IFS= read -r patch; do
	name=$(basename -- "$patch")
	if ! git -C "$kernel" apply --check "$patch"; then
		echo "apply-series: $name does not apply; stopped after $count patch(es)." >&2
		echo "apply-series: the tree is partially patched; start again from a pristine copy." >&2
		exit 1
	fi
	if ! git -C "$kernel" apply --whitespace=error-all "$patch"; then
		echo "apply-series: applying $name failed after $count patch(es); tree is partial." >&2
		exit 1
	fi
	count=$((count + 1))
done < "$list"
echo "apply-series: applied $count patch(es) to $kernel"
