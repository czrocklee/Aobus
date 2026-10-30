"""Probe isolation in actual CMake codemodel replies, including indirect edges."""

import json
import tempfile
import unittest
from pathlib import Path

from ao.core import winui

GOOD = {
    "aobus-winui": ("aobus-winui-lib",),
    "aobus-winui-lib": (),
    "aobus-winui-check": ("aobus-winui", "ao_winui_soul_probe", "ao_winui_track_item_probe"),
    "ao_winui_soul_probe": (),
    "ao_winui_track_item_probe": (),
    "ALL_BUILD": ("aobus-winui",),
}


def write_reply(root, configurations):
    reply = root / ".cmake/api/v1/reply"
    reply.mkdir(parents=True)
    model = {"configurations": []}
    for configuration, graph in configurations.items():
        targets = []
        for name, dependencies in graph.items():
            filename = f"{configuration}-{name}.json"
            record = {"name": name, "dependencies": [{"id": dependency} for dependency in dependencies]}
            (reply / filename).write_text(json.dumps(record), encoding="utf-8")
            targets.append({"id": name, "name": name, "directoryIndex": 0, "jsonFile": filename})
        model["configurations"].append({"name": configuration, "targets": targets})
    (reply / "model.json").write_text(json.dumps(model), encoding="utf-8")
    (reply / "index-2.json").write_text(json.dumps({"reply": {"codemodel-v2": {"jsonFile": "model.json"}}}))
    return reply


class WinUiNativeGraphTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def test_graph_membership_and_transitive_isolation(self):
        variants = [
            ({}, None),
            ({"aobus-winui": ("wrapper",), "wrapper": ("ao_winui_soul_probe",)}, "ao_winui_soul_probe"),
            ({"aobus-winui-check": ("aobus-winui", "ao_winui_soul_probe")}, "ao_winui_track_item_probe"),
            ({"aobus-winui-check": ("ao_winui_soul_probe", "ao_winui_track_item_probe")}, "shipping"),
            ({"ALL_BUILD": ("aobus-winui", "wrapper"), "wrapper": ("ao_winui_soul_probe",)}, "ALL_BUILD"),
            ({"ALL_BUILD": ()}, "omits the shipping"),
        ]
        for index, (changes, error) in enumerate(variants):
            tree = self.root / str(index)
            write_reply(tree, {"Debug": {**GOOD, **changes}})
            with self.subTest(changes=changes):
                if error is None:
                    winui.verify_build_graph(tree, "Debug")
                else:
                    with self.assertRaisesRegex(RuntimeError, error):
                        winui.verify_build_graph(tree, "Debug")

    def test_root_all_build_is_selected_independently_of_target_order(self):
        reply = write_reply(self.root, {"Debug": GOOD})
        model_path = reply / "model.json"
        model = json.loads(model_path.read_text(encoding="utf-8"))
        targets = model["configurations"][0]["targets"]
        root_default = next(target for target in targets if target["name"] == "ALL_BUILD")
        child_default = {"id": "child-all", "name": "ALL_BUILD", "directoryIndex": 1, "jsonFile": "child-all.json"}
        (reply / "child-all.json").write_text(json.dumps({"name": "ALL_BUILD", "dependencies": []}))
        targets.append(child_default)
        for _ in range(2):
            model_path.write_text(json.dumps(model))
            winui.verify_build_graph(self.root, "Debug")
            targets.reverse()
        for root_directory, child_directory in ((1, 1), (0, 0)):
            with self.subTest(root=root_directory, child=child_directory):
                root_default["directoryIndex"] = root_directory
                child_default["directoryIndex"] = child_directory
                model_path.write_text(json.dumps(model))
                with self.assertRaisesRegex(RuntimeError, "one root ALL_BUILD"):
                    winui.verify_build_graph(self.root, "Debug")

    def test_requested_configuration_and_latest_index_govern(self):
        bad = {**GOOD, "aobus-winui": ("ao_winui_soul_probe",)}
        reply = write_reply(self.root, {"Debug": bad, "Release": GOOD})
        (reply / "index-1.json").write_text("invalid old reply")
        winui.verify_build_graph(self.root, "Release")
        with self.assertRaises(RuntimeError):
            winui.verify_build_graph(self.root, "Debug")
        (reply / "index-3.json").write_text("invalid newest reply")
        with self.assertRaises(RuntimeError):
            winui.verify_build_graph(self.root, "Release")

    def test_unavailable_or_invalid_graphs_fail_closed(self):
        with self.assertRaises(RuntimeError):
            winui.verify_build_graph(self.root, "Debug")
        empty = self.root / "no-probes"
        write_reply(empty, {"Debug": {"aobus-winui": (), "aobus-winui-check": (), "ALL_BUILD": ()}})
        with self.assertRaises(RuntimeError):
            winui.verify_build_graph(empty, "Debug")
        reply = write_reply(self.root, {"Release": GOOD})
        with self.assertRaises(RuntimeError):
            winui.verify_build_graph(self.root, "Debug")
        for contents in ("not json", "[]", '{"configurations": []}'):
            with self.subTest(contents=contents):
                (reply / "model.json").write_text(contents)
                with self.assertRaises(RuntimeError):
                    winui.verify_build_graph(self.root, "Release")
