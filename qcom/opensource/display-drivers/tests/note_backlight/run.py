#!/usr/bin/env python3
"""Compile and execute the real Note backlight setter and driver helpers."""
from pathlib import Path
import os, subprocess, tempfile, re, argparse
TEST_DIR = Path(__file__).resolve().parent
SRC = Path(os.environ.get("NOTE_BACKLIGHT_SRC", Path(__file__).resolve().parents[2] / "msm/dsi"))
parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, default=SRC)
SRC = parser.parse_args().source
CC = os.environ.get("CC", "gcc")
def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)+3]
source = (SRC / "dsi_panel.c").read_text()
code = (TEST_DIR / "host.c").read_text()
code = code.replace("/* REAL_SETTER */", function(source, "int dsi_panel_set_backlight("))
def strip_includes(text):
    return re.sub(r"^#include.*$", "", text, flags=re.M)
for marker, filename in (("CORE_HEADER", "dsi_note_backlight_core.h"),
                         ("BL_HEADER", "dsi_note_backlight.h"),
                         ("HBM_HEADER", "dsi_note_hbm_core.h"),
                         ("CORE_SOURCE", "dsi_note_backlight_core.c"),
                         ("ADAPTER_SOURCE", "dsi_note_backlight.c")):
    code = code.replace("/* " + marker + " */", strip_includes((SRC / filename).read_text()))
hbm = (SRC / "dsi_note_hbm.c").read_text()
hbm_core = (SRC / "dsi_note_hbm_core.c").read_text()
code = code.replace("/* HBM_INIT */", function(hbm, "void dsi_note_hbm_init("))
code = code.replace("/* REAL_RESTORE */", function(hbm, "static int note_restore("))
code = code.replace("/* HBM_CORE */", strip_includes(hbm_core))
code = code.replace("/* HBM_ADFR */", "\n".join(function(hbm, signature) for signature in (
    "static struct dsi_panel_cmd_set *note_set(", "static int note_check_set(", "static int note_check_packet(",
    "static int note_send(", "int dsi_note_hbm_backlight_adfr(")))
code = code.replace("/* HBM_INVALIDATE */", "\n".join([
    
    function(hbm, "void dsi_note_hbm_invalidate("),
    function(hbm, "void dsi_note_hbm_low_power(")]))
code = code.replace("/* REAL_LIFECYCLE */", "\n".join(function(source, "int " + name + "(") for name in (
    "dsi_panel_enable", "dsi_panel_disable", "dsi_panel_set_lp1", "dsi_panel_set_lp2", "dsi_panel_set_nolp",
    "dsi_panel_switch_cmd_mode_out", "dsi_panel_switch_video_mode_out", "dsi_panel_switch_video_mode_in",
    "dsi_panel_switch_cmd_mode_in", "dsi_panel_switch", "dsi_panel_post_switch")))
code = code.replace("/* FIXTURE */", (TEST_DIR / "fixture.h").read_text())
code = code.replace("/* REAL_UNPREPARE */", function((SRC / "dsi_ctrl.c").read_text(), "int dsi_ctrl_transfer_unprepare("))
code = code.replace("/* REAL_HOST */", function((SRC / "dsi_display.c").read_text(), "int dsi_host_transfer_sub("))
with tempfile.TemporaryDirectory(prefix="note-backlight-") as temporary:
    path = Path(temporary)
    (path / "host.c").write_text(code)
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", str(path / "host.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)

parser_code = (TEST_DIR / "parser-host.c").read_text()
def block(text, start):
    begin = text.index(start)
    return text[begin:text.index("\n};", begin)+3]
parser_code = parser_code.replace("/* ENUM */", block((SRC / "dsi_defs.h").read_text(), "enum dsi_cmd_set_type {"))
parser_code = parser_code.replace("/* CORE_HEADER */", strip_includes((SRC / "dsi_note_backlight_core.h").read_text()))
parser_code = parser_code.replace("/* CHECK_BLOB */", function((SRC / "dsi_note_backlight_core.c").read_text(), "int note_bl_check_blob("))
parser_code = parser_code.replace("/* PARSE_NOTE */", function((SRC / "dsi_note_backlight.c").read_text(), "int dsi_note_backlight_parse("))
parser_code = parser_code.replace("/* MAPS */", "\n".join(block(source, "const char *"+name) for name in ("cmd_set_prop_map[", "cmd_set_state_map[")))
parser_code = parser_code.replace("/* PARSER */", "\n".join(function(source, signature) for signature in (
    "int dsi_panel_get_cmd_pkt_count(", "int dsi_panel_create_cmd_packets(",
    "void dsi_panel_destroy_cmd_packets(", "void dsi_panel_dealloc_cmd_packets(",
    "int dsi_panel_alloc_cmd_packets(", "static int dsi_panel_parse_cmd_sets_sub(", "static int dsi_panel_parse_cmd_sets(")))
parser_code = parser_code.replace("/* FIXTURE */", (TEST_DIR / "fixture.h").read_text())
with tempfile.TemporaryDirectory(prefix="note-backlight-parser-") as temporary:
    path = Path(temporary)
    (path / "host.c").write_text(parser_code)
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare", str(path / "host.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
