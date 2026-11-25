"""Release packaging checks; run with python -m unittest discover -s scripts/ci."""

import tempfile
import unittest
from pathlib import Path

from collect_release import ASSET_BASENAME, asset_name, collect


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.artifacts = self.root / "artifacts"
        self.output = self.root / "dist"
        build = self.artifacts / "switch"
        build.mkdir(parents=True)
        self.nro = build / f"{ASSET_BASENAME}.nro"
        self.nro.write_bytes(b"NRO0-release")

    def test_asset_name_carries_the_version(self):
        self.assertEqual(asset_name("v0.2.0"), "LevelUp-NX-v0.2.0.nro")

    def test_collects_only_the_versioned_switch_nro(self):
        assets = collect("v1.6.0", self.artifacts, self.output)
        self.assertEqual([asset.name for asset in assets], ["LevelUp-NX-v1.6.0.nro"])
        self.assertEqual(
            [p.name for p in self.output.iterdir()], ["LevelUp-NX-v1.6.0.nro"]
        )
        self.assertEqual(
            (self.output / "LevelUp-NX-v1.6.0.nro").read_bytes(), b"NRO0-release"
        )

    def test_ignores_foreign_platform_artifacts(self):
        # 上游 Moonlight 的多平台产物如果混进了 artifacts 目录，绝不能进 Release
        for name in (
            "Moonlight-Linux-x86_64.deb",
            "Moonlight-Windows-x64.zip",
            "Moonlight-Switch2.zip",
            "Moonlight-Switch1.nro",
            "Debug.elf",
        ):
            (self.artifacts / name).write_bytes(b"leftover")
        assets = collect("v1.6.0", self.artifacts, self.output)
        self.assertEqual([asset.name for asset in assets], ["LevelUp-NX-v1.6.0.nro"])
        self.assertEqual(
            [p.name for p in self.output.iterdir()], ["LevelUp-NX-v1.6.0.nro"]
        )

    def test_rejects_non_release_tags(self):
        for tag in ("v1", "1.6.0", "v1.6.0-rc1", "v1.6.0/other"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                collect(tag, self.artifacts, self.output)
        self.assertFalse(self.output.exists())

    def test_requires_the_nro_artifact(self):
        self.nro.unlink()
        with self.assertRaisesRegex(ValueError, "Missing LevelUp-NX.nro"):
            collect("v1.6.0", self.artifacts, self.output)
        self.assertFalse(self.output.exists())

    def test_refuses_ambiguous_nro(self):
        (self.artifacts / "LevelUp-NX-copy.nro").write_bytes(b"second")
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            collect("v1.6.0", self.artifacts, self.output)

    def test_refuses_to_overwrite_assets(self):
        self.output.mkdir()
        asset = self.output / "LevelUp-NX-v1.6.0.nro"
        asset.write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "already exists"):
            collect("v1.6.0", self.artifacts, self.output)
        self.assertEqual(asset.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
