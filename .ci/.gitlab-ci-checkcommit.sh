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

resolve_base_ref() {
	if [ -n "${BASE_REF:-}" ]; then
		echo "$BASE_REF"
		return 0
	fi

	for sha in "${CI_MERGE_REQUEST_DIFF_BASE_SHA:-}" "${CI_MERGE_REQUEST_TARGET_BRANCH_SHA:-}"; do
		if [ -n "$sha" ] && git cat-file -e "${sha}^{commit}" 2>/dev/null; then
			echo "$sha"
			return 0
		fi
	done

	local attempt=0
	local delay=5
	until git fetch https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer.git; do
		attempt=$((attempt + 1))
		if [ "$attempt" -ge 4 ]; then
			echoerr "git fetch from gitlab.freedesktop.org failed after $attempt attempts"
			return 1
		fi
		echoerr "git fetch failed; retrying in ${delay}s (attempt ${attempt}/4)..."
		sleep "$delay"
		delay=$((delay * 2))
	done
	echo "FETCH_HEAD"
}

base_ref=$(resolve_base_ref)
merge_base=$(git merge-base "$base_ref" HEAD)

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

python3 ./.ci/check-build-sync.py .
