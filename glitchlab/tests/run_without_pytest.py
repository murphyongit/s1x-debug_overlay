"""Runs tests/test_*.py when pytest isn't installed:  python tests/run_without_pytest.py

Supports the small part of pytest the tests use: pytest.raises and the tmp_path / monkeypatch fixtures."""
from __future__ import annotations

import contextlib
import importlib.util
import inspect
import sys
import tempfile
import traceback
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

if "pytest" not in sys.modules:
    try:
        import pytest  # noqa: F401
    except ImportError:
        fake = types.ModuleType("pytest")

        @contextlib.contextmanager
        def raises(exc):
            try:
                yield
            except exc:
                return
            raise AssertionError(f"did not raise {exc.__name__}")

        fake.raises = raises
        sys.modules["pytest"] = fake


class MonkeyPatch:
    def __init__(self):
        self._undo = []

    def setattr(self, obj, name, value):
        self._undo.append((obj, name, getattr(obj, name)))
        setattr(obj, name, value)

    def undo(self):
        for obj, name, old in reversed(self._undo):
            setattr(obj, name, old)


def main() -> int:
    failed = passed = 0
    for path in sorted((ROOT / "tests").glob("test_*.py")):
        spec = importlib.util.spec_from_file_location(path.stem, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        for name, fn in inspect.getmembers(mod, inspect.isfunction):
            if not name.startswith("test_"):
                continue
            mp = MonkeyPatch()
            with tempfile.TemporaryDirectory() as td:
                kwargs = {}
                params = inspect.signature(fn).parameters
                if "tmp_path" in params:
                    kwargs["tmp_path"] = Path(td)
                if "monkeypatch" in params:
                    kwargs["monkeypatch"] = mp
                try:
                    fn(**kwargs)
                    passed += 1
                    print(f"ok    {name}")
                except Exception:
                    failed += 1
                    print(f"FAIL  {name}")
                    traceback.print_exc()
                finally:
                    mp.undo()
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
