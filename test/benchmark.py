import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("benchmark", Path(__file__).resolve().parents[1] / "bench/bench.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FakeBenchmark(module.Benchmark):
    def __init__(self, output):
        super().__init__(output, 0.001, 1)
        self.state = dict(source_path="/original.mp4", loaded_path="/original.mp4", loaded_kind="video",
                          manual_pause=True, idle_pause=False, always_animate=False, render_alive=True,
                          engine="renderer", media_ready=True, locked=False)
        self.samples = []
        self.fail_at = None

    def request(self, command, renderer=False, **fields):
        if renderer:
            return 2, dict(status="ok", ready=True, has_video=True, paused=self.paused(), time_pos=0)
        if command in ("pause", "resume"):
            self.state["manual_pause"] = command == "pause"
        elif command in ("idle-pause", "idle-resume"):
            self.state["idle_pause"] = command == "idle-pause"
        elif command == "always-animate":
            self.state["always_animate"] = fields["value"]
        elif command == "set":
            self.state.update(source_path=fields["path"], engine="shell", render_alive=False)
        return 1, dict(self.state, status="ok", paused=self.paused(), reason=self.reason())

    def reason(self):
        if self.state["manual_pause"]:
            return "manual"
        if self.state["always_animate"]:
            return "always-animate"
        return "idle" if self.state["idle_pause"] else "visible"

    def paused(self):
        return self.reason() in ("manual", "idle")

    @staticmethod
    def process(pid):
        return dict(pid=pid, started=1, parent=1, ticks=0, rss=1)

    def sample(self, state):
        self.samples.append(state)
        if state == self.fail_at:
            raise RuntimeError("injected failure")
        super().sample(state)


class BenchmarkTest(unittest.TestCase):
    def test_samples_and_restore_initial_pause(self):
        with tempfile.TemporaryDirectory() as directory:
            bench = FakeBenchmark(directory)
            bench.run()
            self.assertEqual(bench.samples, ["playing", "paused", "policy"])
            self.assertTrue(bench.state["manual_pause"])
            self.assertFalse(bench.state["always_animate"])
            self.assertFalse(bench.state["idle_pause"])

    def test_restore_after_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            bench = FakeBenchmark(directory)
            bench.state["idle_pause"] = True
            bench.fail_at = "policy"
            with self.assertRaises(RuntimeError):
                bench.run()
            self.assertTrue(bench.state["manual_pause"])
            self.assertTrue(bench.state["idle_pause"])
            self.assertFalse(bench.state["always_animate"])

    def test_restore_after_interruption(self):
        with tempfile.TemporaryDirectory() as directory:
            bench = FakeBenchmark(directory)
            def interrupted(_state):
                raise SystemExit(143)
            bench.sample = interrupted
            with self.assertRaises(SystemExit):
                bench.run()
            self.assertTrue(bench.state["manual_pause"])
            self.assertFalse(bench.state["always_animate"])
            self.assertFalse(bench.state["idle_pause"])

    def test_still_measures_daemon_without_renderer(self):
        with tempfile.TemporaryDirectory() as directory:
            bench = FakeBenchmark(directory)
            bench.state.update(engine="shell", render_alive=False)
            bench.sample("still")
            self.assertTrue((Path(directory) / "still.json").exists())
            bench.state["render_alive"] = True
            with self.assertRaises(RuntimeError):
                bench.snapshot("still")

    def test_reject_paused_playing_sample(self):
        with tempfile.TemporaryDirectory() as directory:
            bench = FakeBenchmark(directory)
            bench.command("always-animate", value=True)
            with self.assertRaises(RuntimeError):
                bench.snapshot("playing")

    def test_reject_restart_or_media_change(self):
        with tempfile.TemporaryDirectory() as directory:
            bench = FakeBenchmark(directory)
            bench.command("resume")
            bench.command("always-animate", value=True)
            before = bench.snapshot("playing")
            after = copy.deepcopy(before)
            after["processes"]["renderer"]["started"] += 1
            with self.assertRaises(RuntimeError):
                bench.verify_same(before, after)
            after = copy.deepcopy(before)
            after["daemon"]["loaded_path"] = "/replacement.mp4"
            with self.assertRaises(RuntimeError):
                bench.verify_same(before, after)


if __name__ == "__main__":
    unittest.main()
