#! /usr/bin/env bash

set -euo pipefail

check_tool_installed() {
	if ! command -v $1 &> /dev/null
	then
		echo "Please install '$1' tool"
		exit 1
	fi
}

echoerr() {
	printf "ERROR: %s\n" "$*" >&2
}

findtag() {
	local commit_body tag person
	commit_body=$1
	tag=$2
	person=$3

	# trim duplicate spaces from commit body and person
	match="$tag: $(echo $person | tr -s ' ')"

	if [ -z "$(echo "$commit_body" | tr -s ' ' | grep -i "$match")" ]; then
		echoerr "Tag is missing from commit body"
		echoerr ""
		echoerr "Looking for '"$match"' in: "
		echoerr "-----------------------------------------------------"
		echoerr "$commit_body"
		echoerr "-----------------------------------------------------"
		echoerr ""
		return 0
	fi

	return 1
}

check_tool_installed bpfmt
check_tool_installed clang-format-diff-19

git fetch https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer.git
merge_base=$(git merge-base FETCH_HEAD HEAD)

# Validate the MR diff against merge-base rather than diffing each commit
# against the working tree (where git diff HEAD -- is empty on single-commit MRs).
git diff -U0 "${merge_base}..HEAD" -- | clang-format-diff-19 -p 1 -style=file > /tmp/format-fixup.patch
if [ -s /tmp/format-fixup.patch ]; then
	cat /tmp/format-fixup.patch >&2
	exit 1
fi

find . -name "*.bp" -exec bpfmt -d -s {} + > /tmp/bpfmt.patch
if [ -s /tmp/bpfmt.patch ]; then
	cat /tmp/bpfmt.patch >&2
	exit 1
fi
