"""Shared helpers for the tools/nid tests (imported by the test_*.py files).

The tools are loaded from their files, so the tests work under `python -I` and whatever the
current directory is. Nothing here touches the network.
"""

import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_DIR = os.path.dirname(HERE)
DATA = os.path.join(HERE, "data")
REPO = os.path.normpath(os.path.join(TOOL_DIR, "..", ".."))
TRIAGE_DIR = os.path.normpath(os.path.join(TOOL_DIR, "..", "triage"))
TRIAGE_FIXTURES = os.path.join(TRIAGE_DIR, "tests", "fixtures")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


nid = load("kyty_nid_test", os.path.join(TOOL_DIR, "nid.py"))


def load_tool(name):
    """tools/nid/<name>.py as a module."""
    return load("kyty_%s_test" % name, os.path.join(TOOL_DIR, name + ".py"))


def data(name):
    return os.path.join(DATA, name)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def confirmed_pairs():
    """[(nid, name, where the NID is, where the name is)] from data/confirmed_pairs.tsv."""
    rows = []
    with open(data("confirmed_pairs.tsv"), "r", encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            rows.append(tuple(line.rstrip("\n").split("\t")))
    return rows


def pair_name(nid_text):
    for row in confirmed_pairs():
        if row[0] == nid_text:
            return row[1]
    raise KeyError(nid_text)


def in_checkout():
    return os.path.isdir(os.path.join(REPO, "src", "libs"))
