#!/usr/bin/env python
"""Tests remixes end to end with a native blastem running headless.

Usage: python tests/remix_test.py [path to blastem]

Needs a build from ./build_native, run from anywhere. The game is generated here, so no
ROM is needed: it adds 1 to the word at $FF0000 each frame and does nothing else.
"""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

COUNTER = "0xFF0000"
BLASTEM = None
FRAMES = 240


def make_rom():
    code = bytes.fromhex(
        "33FC814400C00004"                # display on, as the vblank flag stays set while it's off
        "303900C00004" "08000003" "67F4"  # wait for vblank: move.w $C00004,d0; btst #3,d0; beq
        "527900FF0000"                    # addq.w #1,$FF0000
        "303900C00004" "08000003" "66F4"  # wait for vblank to end
        "60E0"                            # bra back to the start
    )
    rom = bytearray(0x10000)
    rom[0:8] = struct.pack(">II", 0x01000000, 0x200)
    rom[0x100:0x110] = b"SEGA GENESIS    "
    rom[0x1F0:0x1F1] = b"U"
    rom[0x200:0x200 + len(code)] = code
    return bytes(rom)


def counter_condition(op, value):
    return {"region": "wram", "address": COUNTER, "size": 2, "op": op, "value": value}


def counter_patch(value, **rest):
    return {"region": "wram", "address": COUNTER, "size": 2, "value": value, **rest}


def remix(**rest):
    return {"format": 1, "system": "genesis", **rest}


def outcomes(output):
    return [(m[0], int(m[1])) for m in re.findall(r"Remix outcome: (\w+) at frame (\d+)", output)]


def add_chunk(quote, tag, data):
    return quote + tag + struct.pack("<I", len(data)) + data


class RemixTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.rom = self.path("game.bin")
        with open(self.rom, "wb") as f:
            f.write(make_rom())

    def path(self, name):
        return os.path.join(self.dir.name, name)

    def run_blastem(self, *args, frames=FRAMES, rom=None):
        result = subprocess.run(
            [BLASTEM, rom or self.rom, "-b", str(frames), *args],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120,
        )
        return result.returncode, result.stdout

    def write_remix(self, value, name="remix.json"):
        path = self.path(name)
        with open(path, "w") as f:
            f.write(value if isinstance(value, str) else json.dumps(value))
        return path

    def record(self, value, name="run.brec", frames=FRAMES):
        path = self.path(name)
        code, output = self.run_blastem("-W", path, "-X", self.write_remix(value), frames=frames)
        self.assertEqual(code, 0, output)
        return path, output

    def outcome_of(self, value):
        _, output = self.record(value)
        found = outcomes(output)
        self.assertEqual(len(found), 1, output)
        return found[0]

    def test_counter_counts_frames(self):
        result, frame = self.outcome_of(remix(objective={"win": [counter_condition(">=", 30)]}))
        self.assertEqual(result, "win")
        self.assertLessEqual(abs(frame - 30), 3)

    def test_once_patch_lands_on_its_frame(self):
        self.assertEqual(
            self.outcome_of(remix(
                patches=[counter_patch(1000, mode="once", frame=5)],
                objective={"win": [counter_condition(">=", 1000)]},
            )),
            ("win", 5),
        )

    def test_hold_patch_ends_at_until(self):
        result, frame = self.outcome_of(remix(
            patches=[counter_patch(7, mode="hold", until=50)],
            objective={"win": [counter_condition(">=", 40)]},
        ))
        self.assertEqual(result, "win")
        self.assertLessEqual(abs(frame - 83), 3)

    def test_patch_uses_big_endian_bytes(self):
        patch = {"region": "wram", "address": "0xFF0010", "size": 4, "value": "0x12345678"}
        low_byte = {"region": "wram", "address": "0xFF0013", "op": "==", "value": "0x78"}
        high_byte = {"region": "wram", "address": "0xFF0010", "op": "==", "value": "0x12"}
        self.assertEqual(self.outcome_of(remix(patches=[patch], objective={"win": [low_byte, high_byte]})), ("win", 0))

    def test_conditions_are_anded(self):
        never = {"region": "wram", "address": "0xFF0020", "op": "==", "value": 1}
        _, output = self.record(remix(objective={"win": [counter_condition(">=", 5), never]}))
        self.assertEqual(outcomes(output), [])

    def test_lose_condition(self):
        result, frame = self.outcome_of(remix(objective={"lose": [counter_condition(">=", 20)]}))
        self.assertEqual(result, "lose")
        self.assertLessEqual(abs(frame - 20), 3)

    def test_time_limit_loses(self):
        self.assertEqual(self.outcome_of(remix(objective={"timeLimit": 40, "win": [counter_condition(">=", 60000)]})), ("lose", 40))

    def test_win_beats_lose_on_the_same_frame(self):
        both = [counter_condition(">=", 10)]
        self.assertEqual(self.outcome_of(remix(objective={"win": both, "lose": both}))[0], "win")

    def test_objective_ends_once(self):
        _, output = self.record(remix(objective={"win": [counter_condition(">=", 5)], "timeLimit": 50}))
        self.assertEqual(len(outcomes(output)), 1)

    def test_no_objective_has_no_outcome(self):
        _, output = self.record(remix(patches=[counter_patch(9)]))
        self.assertEqual(outcomes(output), [])

    def test_unknown_keys_are_ignored(self):
        self.assertEqual(self.outcome_of(remix(future={"a": [1, {"b": 2}]}, objective={"timeLimit": 3, "future": 1})), ("lose", 3))

    def test_replay_reaches_the_same_outcome_and_state(self):
        value = remix(patches=[counter_patch(500, frame=70)], objective={"win": [counter_condition(">=", 520)]})
        recording, recorded = self.record(value, frames=200)
        remix_file = self.write_remix(value)
        first_code, first = self.run_blastem("-R", recording, "-X", remix_file, frames=260)
        second_code, second = self.run_blastem("-R", recording, "-X", remix_file, frames=260)
        self.assertEqual((first_code, second_code), (0, 0), first + second)
        self.assertEqual(outcomes(first), outcomes(recorded), first)
        self.assertEqual(outcomes(second), outcomes(recorded), second)
        self.assertNotIn("desynced", first)
        states = [re.findall(r"Playback finished in state (\w+)", text) for text in (first, second)]
        self.assertTrue(states[0], first)
        self.assertEqual(states[0], states[1])

    def test_replay_without_the_remix_desyncs(self):
        value = remix(patches=[counter_patch(500, frame=10)])
        recording, _ = self.record(value, frames=200)
        _, output = self.run_blastem("-R", recording, frames=260)
        self.assertIn("desynced", output)

    def test_no_remix_runs_unchanged(self):
        recording = self.path("plain.brec")
        code, output = self.run_blastem("-W", recording, frames=200)
        self.assertEqual(code, 0, output)
        code, output = self.run_blastem("-R", recording, frames=260)
        self.assertEqual(code, 0, output)
        self.assertNotIn("desynced", output)
        self.assertEqual(outcomes(output), [])

    def assert_rejected(self, value, message):
        code, output = self.run_blastem("-X", self.write_remix(value))
        self.assertNotEqual(code, 0, output)
        self.assertIn(message, output)

    def test_rejects_bad_remixes(self):
        cases = [
            ("not json", "valid JSON"),
            ("", "valid JSON"),
            ("[1]", "one JSON object"),
            ("{} {}", "one JSON object"),
            ("[" * 60000, "valid JSON"),
            (json.dumps({"system": "genesis"}), "needs a format"),
            (remix(format=2), "format 2"),
            (remix(system="sms"), "isn't for this system"),
            (remix(patches={}), "isn't a list"),
            (remix(patches=[1]), "isn't an object"),
            (remix(patches=[{"region": "wram", "value": 1}]), "needs an address"),
            (remix(patches=[{"address": COUNTER, "value": 1}]), "has no region"),
            (remix(patches=[counter_patch(1, region="vram")]), "isn't a region"),
            (remix(patches=[counter_patch(1, address="0xE00000")]), "isn't in wram"),
            (remix(patches=[counter_patch(1, address="0xFFFFFF")]), "isn't in wram"),
            (remix(patches=[counter_patch(70000)]), "doesn't fit"),
            (remix(patches=[counter_patch(1, size=3)]), "size must be"),
            (remix(patches=[counter_patch(1, mode="sometimes")]), "mode must be"),
            (remix(patches=[counter_patch(1, mode="hold", frame=9, until=9)]), "until must be"),
            (remix(patches=[counter_patch(-1)]), "needs a value"),
            (remix(patches=[counter_patch(1.5)]), "needs a value"),
            (remix(patches=[counter_patch(99999999999)]), "needs a value"),
            (remix(patches=[counter_patch(1)] * 257), "patches at most"),
            (remix(objective={"win": [counter_condition("~", 1)]}), "op must be"),
            (remix(objective={"win": [counter_condition("==", 1)] * 65}), "conditions at most"),
            (remix(objective={"timeLimit": "soon"}), "whole number"),
            (" " * (64 * 1024 + 1), "bytes at most"),
        ]
        for value, message in cases:
            with self.subTest(message=message, value=str(value)[:40]):
                self.assert_rejected(value, message)

    def make_quote(self, value):
        """a quote whose recording ran with the remix, without an RMIX chunk yet"""
        recording, recorded = self.record(value, frames=200)
        quote = self.path("quote.pq")
        code, output = self.run_blastem("-R", recording, "-X", self.write_remix(value), "-Q", quote, frames=260)
        self.assertEqual(code, 0, output)
        with open(quote, "rb") as f:
            return f.read(), recorded

    def test_quote_plays_its_remix(self):
        value = remix(patches=[counter_patch(500, frame=70)], objective={"win": [counter_condition(">=", 520)]})
        quote, recorded = self.make_quote(value)
        path = self.path("remixed.pq")
        with open(path, "wb") as f:
            f.write(add_chunk(quote, b"RMIX", json.dumps(value).encode()))
        code, output = self.run_blastem(frames=260, rom=path)
        self.assertEqual(code, 0, output)
        self.assertEqual(outcomes(output), outcomes(recorded), output)
        self.assertNotIn("desynced", output)

    def test_quote_with_a_bad_remix_says_so(self):
        quote, _ = self.make_quote(remix())
        path = self.path("bad.pq")
        with open(path, "wb") as f:
            f.write(add_chunk(quote, b"RMIX", b'{"format": 99}'))
        code, output = self.run_blastem(frames=260, rom=path)
        self.assertNotEqual(code, 0, output)
        self.assertIn("format 99", output)

    def test_quote_without_a_remix_is_unaffected(self):
        quote, _ = self.make_quote(remix())
        path = self.path("plain.pq")
        with open(path, "wb") as f:
            f.write(quote)
        code, output = self.run_blastem(frames=260, rom=path)
        self.assertEqual(code, 0, output)
        self.assertEqual(outcomes(output), [])


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default = os.path.join(root, "blastem.exe" if os.name == "nt" else "blastem")
    BLASTEM = os.path.abspath(sys.argv.pop(1)) if len(sys.argv) > 1 else default
    unittest.main(verbosity=2)
