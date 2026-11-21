import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import evaluate_openzdm_boost as study


class ResearchFreezeTests(unittest.TestCase):
    def test_historical_source_is_verified_against_original_bytes(self):
        relative = "tools/openzdm_boost/PooledTexture.h"
        recorded = b"frozen source\r\n"
        frozen = {"hashes": {str(study.ROOT / relative): hashlib.sha256(recorded).hexdigest()}}
        with mock.patch.object(study.subprocess, "run") as git:
            git.return_value.stdout = b"frozen source\n"
            study.verify_research_freeze(frozen)
        self.assertEqual(git.call_args.args[0][2], f"{study.PRE_SHARED_MODEL_REVISION}:{relative}")

    def test_current_freeze_requires_current_source_bytes(self):
        relative = "tools/openzdm_boost/PooledTexture.h"
        frozen = {"source_layout": "shared", "hashes": {str(study.ROOT / relative): "0" * 64}}
        with mock.patch.object(study.subprocess, "run") as git:
            with self.assertRaises(ValueError):
                study.verify_research_freeze(frozen)
            git.assert_not_called()

    def test_historical_model_cannot_bypass_checksum_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.txt"
            model.write_bytes(b"changed model")
            frozen = {"hashes": {str(model): "0" * 64}}
            with mock.patch.object(study.subprocess, "run") as git:
                with self.assertRaises(ValueError):
                    study.verify_research_freeze(frozen)
                git.assert_not_called()
