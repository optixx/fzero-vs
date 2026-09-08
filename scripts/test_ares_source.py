"""Offline tests for source preservation and ordered patch application."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from ares_source import apply_patches, fetch, git, link_binary


def patch(before, after):
    return f"diff --git a/value b/value\n--- a/value\n+++ b/value\n@@ -1 +1 @@\n-{before}\n+{after}\n"


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fzvs source ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "vendor/ares"
        self.source.mkdir(parents=True)
        self.patches = self.root / "patches/ares"
        self.patches.mkdir(parents=True)
        (self.patches / "series").write_text("")
        git(self.source, "init", "-q")
        git(self.source, "config", "user.name", "Test")
        git(self.source, "config", "user.email", "test@example.invalid")
        git(self.source, "remote", "add", "origin", "https://example.invalid/ares.git")
        (self.source / "value").write_text("one\n")
        git(self.source, "add", "value")
        git(self.source, "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        self.pin = {"commit": git(self.source, "rev-parse", "HEAD"), "tag": "test",
                    "repository": "https://example.invalid/ares.git"}

    def test_launcher_preserves_arguments_cwd_and_replaces_old_symlink(self):
        executable = self.root / ("build/ares/desktop-ui/ares.app/Contents/MacOS/ares"
            if sys.platform == "darwin" else "build/ares/desktop-ui/ares")
        executable.parent.mkdir(parents=True)
        executable.write_text('#!/bin/sh\nprintf "%s\\n" "$0" "$PWD" "$@"\n')
        executable.chmod(0o755)
        launcher = self.root / "build/bin/ares"
        launcher.parent.mkdir(parents=True)
        launcher.symlink_to(executable)
        link_binary(self.root)
        link_binary(self.root)  # a repeat build is idempotent
        self.assertFalse(launcher.is_symlink())
        args = ['roms/F-ZERO (U) [!].smc', 'room $key `literal`', '']
        lines = subprocess.check_output([str(launcher), *args], cwd=self.root, text=True).splitlines()
        self.assertEqual(Path(lines[0]).resolve(), executable.resolve())
        self.assertEqual(Path(lines[1]).resolve(), self.root.resolve())
        self.assertEqual(lines[2:], args)

    def series(self, *changes):
        names = []
        for index, (before, after) in enumerate(changes):
            name = f"{index}.patch"
            (self.patches / name).write_text(patch(before, after))
            names.append(name)
        (self.patches / "series").write_text("\n".join(names) + "\n")

    def test_dependent_series_applies_once_and_preserves_edits(self):
        self.series(("one", "two"), ("two", "three"))
        apply_patches(self.root, self.pin)
        self.assertEqual((self.source / "value").read_text(), "three\n")
        (self.source / "value").write_text("local change\n")
        apply_patches(self.root, self.pin)
        self.assertEqual((self.source / "value").read_text(), "local change\n")

    def test_conflict_does_not_partially_apply(self):
        self.series(("one", "two"), ("missing", "three"))
        with self.assertRaises(subprocess.CalledProcessError):
            apply_patches(self.root, self.pin)
        self.assertEqual((self.source / "value").read_text(), "one\n")
        self.assertEqual(git(self.source, "status", "--porcelain"), "")

    def test_changed_series_refuses_to_discard_existing_changes(self):
        self.series(("one", "two"))
        apply_patches(self.root, self.pin)
        self.series(("one", "three"))
        with self.assertRaisesRegex(RuntimeError, "local changes"):
            apply_patches(self.root, self.pin)
        self.assertEqual((self.source / "value").read_text(), "two\n")

    def test_fetch_reuses_pinned_checkout_without_network(self):
        (self.source / "value").write_text("uncommitted\n")
        fetch(self.root, self.pin)
        self.assertEqual((self.source / "value").read_text(), "uncommitted\n")
        with self.assertRaisesRegex(RuntimeError, "HEAD differs"):
            fetch(self.root, dict(self.pin, commit="0" * 40))

    def test_rejects_patch_outside_series_directory(self):
        (self.root / "outside.patch").write_text(patch("one", "two"))
        (self.patches / "series").write_text("../../outside.patch\n")
        with self.assertRaisesRegex(RuntimeError, "Invalid or missing"):
            apply_patches(self.root, self.pin)

    def test_empty_series_can_be_extended_on_clean_checkout(self):
        apply_patches(self.root, self.pin)
        self.series(("one", "two"))
        apply_patches(self.root, self.pin)
        marker = json.loads((self.source / ".git/fzvs-patches.json").read_text())
        self.assertEqual(len(marker["patches"]), 1)


if __name__ == "__main__":
    unittest.main()
