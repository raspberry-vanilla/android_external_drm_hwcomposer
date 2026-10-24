#!/usr/bin/env python3
"""Verifies build-graph source synchronization across Android.bp, .ci/Makefile.unittests, meson.build, and git."""

import os
import re
import subprocess
import sys

MAKEFILE_ONLY_ALLOWLIST = {
    "tests/host_compat_stubs.cpp",
    "utils/LibdisplayEdidWrapper.cpp",
    "utils/LibdisplayEdidWrapperTest.cpp",
}

MESON_ONLY_ALLOWLIST = {
    "bufferinfo/legacy/BufferInfoMinigbm.cpp",
}


def extract_bp_block_srcs(bp_text: str, block_name: str) -> set[str]:
  pattern = re.compile(
      r'name:\s*"' + re.escape(block_name) + r'".*?srcs:\s*\[(.*?)\]',
      re.DOTALL,
  )
  match = pattern.search(bp_text)
  if not match:
    raise RuntimeError(f"Could not find srcs for {block_name} in Android.bp")
  return set(re.findall(r'"([^"]+\.cpp)"', match.group(1)))


def extract_all_bp_srcs(repo_root: str) -> set[str]:
  srcs: set[str] = set()
  bp_files_out = subprocess.check_output(
      ["git", "-C", repo_root, "ls-files", "*Android.bp"], text=True
  )
  for rel_bp in bp_files_out.splitlines():
    rel_bp = rel_bp.strip()
    if not rel_bp:
      continue
    rel_dir = os.path.dirname(rel_bp)
    bp_path = os.path.join(repo_root, rel_bp)
    with open(bp_path, "r", encoding="utf-8") as f:
      text = f.read()
    for m in re.findall(r'"([^"]+\.cpp)"', text):
      norm = os.path.normpath(m)
      if rel_dir and not os.path.exists(os.path.join(repo_root, norm)):
        cand = os.path.normpath(os.path.join(rel_dir, m))
        if os.path.exists(os.path.join(repo_root, cand)):
          norm = cand
      srcs.add(norm)
  return srcs


def extract_makefile_unittest_srcs(mk_text: str) -> set[str]:
  match = re.search(
      r"UNITTEST_SRCS\s*:=\s*((?:.*\\\n)*.*)", mk_text, re.MULTILINE
  )
  if not match:
    raise RuntimeError("Could not find UNITTEST_SRCS in .ci/Makefile.unittests")
  block = match.group(1).replace("\\", " ")
  return {tok.strip() for tok in block.split() if tok.strip().endswith(".cpp")}


def extract_meson_srcs(repo_root: str) -> set[str]:
  srcs: set[str] = set()
  meson_files_out = subprocess.check_output(
      ["git", "-C", repo_root, "ls-files", "*meson.build"], text=True
  )
  for rel_meson in meson_files_out.splitlines():
    rel_meson = rel_meson.strip()
    if not rel_meson:
      continue
    rel_dir = os.path.dirname(rel_meson)
    meson_path = os.path.join(repo_root, rel_meson)
    with open(meson_path, "r", encoding="utf-8") as f:
      text = f.read()
    for m in re.findall(r"'([^']+\.cpp)'", text):
      norm = os.path.normpath(os.path.join(rel_dir, m))
      srcs.add(norm)
  return srcs


def main() -> int:
  repo_root = sys.argv[1] if len(sys.argv) > 1 else "."
  bp_path = os.path.join(repo_root, "Android.bp")
  mk_path = os.path.join(repo_root, ".ci", "Makefile.unittests")

  with open(bp_path, "r", encoding="utf-8") as f:
    bp_text = f.read()
  with open(mk_path, "r", encoding="utf-8") as f:
    mk_text = f.read()

  bp_ut = extract_bp_block_srcs(bp_text, "drm_hwcomposer_unittests")
  mk_ut = extract_makefile_unittest_srcs(mk_text)

  errors: list[str] = []

  # 1. Compare Android.bp drm_hwcomposer_unittests vs .ci/Makefile.unittests UNITTEST_SRCS
  only_in_bp = sorted(bp_ut - mk_ut)
  only_in_mk = sorted((mk_ut - bp_ut) - MAKEFILE_ONLY_ALLOWLIST)
  if only_in_bp:
    errors.append(
        "Files in Android.bp (drm_hwcomposer_unittests) missing from"
        f" .ci/Makefile.unittests: {only_in_bp}"
    )
  if only_in_mk:
    errors.append(
        "Files in .ci/Makefile.unittests (UNITTEST_SRCS) missing from"
        f" Android.bp: {only_in_mk}"
    )

  # 2. Check that all tracked *Test*.cpp files in git are wired into unit tests
  git_tests_out = subprocess.check_output(
      ["git", "-C", repo_root, "ls-files", "*Test*.cpp"], text=True
  )
  git_test_files = {
      line.strip()
      for line in git_tests_out.splitlines()
      if line.strip() and not line.strip().startswith("tests/test_include/")
  }
  unwired_tests = sorted(
      (git_test_files - bp_ut) - (mk_ut & MAKEFILE_ONLY_ALLOWLIST)
  )
  if unwired_tests:
    errors.append(
        f"Tracked *Test*.cpp files not wired into drm_hwcomposer_unittests: {unwired_tests}"
    )

  # 3. Check that production sources in Android.bp and meson.build stay in sync
  bp_common = extract_bp_block_srcs(bp_text, "drm_hwcomposer_common")
  bp_hwc3 = extract_bp_block_srcs(bp_text, "drm_hwcomposer_hwc3")
  bp_all_srcs = extract_all_bp_srcs(repo_root)
  meson_srcs = extract_meson_srcs(repo_root)
  missing_in_meson = sorted((bp_common | bp_hwc3) - meson_srcs)
  if missing_in_meson:
    errors.append(
        "Production files in Android.bp filegroups missing from meson.build:"
        f" {missing_in_meson}"
    )
  missing_in_bp = sorted((meson_srcs - bp_all_srcs) - MESON_ONLY_ALLOWLIST)
  if missing_in_bp:
    errors.append(
        "Production files in meson.build missing from Android.bp:"
        f" {missing_in_bp}"
    )

  if errors:
    for err in errors:
      print(f"BUILD_SYNC_ERROR: {err}", file=sys.stderr)
    return 1

  print(
      f"OK (build_sync): {len(bp_ut)} unittest srcs, {len(git_test_files)} test"
      f" files, {len(bp_common | bp_hwc3)} common/hwc3 srcs verified."
  )
  return 0


if __name__ == "__main__":
  sys.exit(main())
