"""Verify that research setup refuses to overwrite a user's dependency checkout."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from prepare_research import require_source


class SourceGuardTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name) / "source"
        self.source.mkdir()
        self.git("init", "--quiet", "--initial-branch=research")
        (self.source / "input.txt").write_text("upstream\n")
        self.git("add", "input.txt")
        self.git("-c", "user.name=Research Fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "--quiet", "-m", "fixture")
        self.commit = self.git("rev-parse", "HEAD")

    def git(self, *arguments):
        return subprocess.check_output(["git", *arguments], cwd=self.source, text=True).strip()

    def test_clean_exact_commit_is_accepted(self):
        require_source(self.source, self.commit)

    def test_wrong_commit_is_preserved(self):
        with self.assertRaisesRegex(RuntimeError, "Wrong source version"):
            require_source(self.source, "0" * 40)
        self.assertEqual(self.git("rev-parse", "HEAD"), self.commit)
        self.assertEqual((self.source / "input.txt").read_text(), "upstream\n")

    def test_local_changes_and_untracked_files_are_preserved(self):
        (self.source / "input.txt").write_text("user changes\n")
        (self.source / "local.txt").write_text("user file\n")
        with self.assertRaisesRegex(RuntimeError, "local changes"):
            require_source(self.source, self.commit)
        self.assertEqual((self.source / "input.txt").read_text(), "user changes\n")
        self.assertEqual((self.source / "local.txt").read_text(), "user file\n")

    def test_parent_git_metadata_is_not_treated_as_a_dependency(self):
        child = self.source / "nested"
        child.mkdir()
        with self.assertRaisesRegex(RuntimeError, "Not an independent"):
            require_source(child, self.commit)
        self.assertEqual(self.git("rev-parse", "HEAD"), self.commit)


if __name__ == "__main__":
    unittest.main()
