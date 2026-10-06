#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile unchanged driver functions, replacing only kernel/hardware boundaries.

This exercises decisions and cleanup in production C, not an independent model.
It cannot prove Linux locking, interrupt-controller, regulator or device behavior.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PLATFORM_FUNCTIONS = (
    "jiiov_resources_drop", "jiiov_resources_request", "jiiov_power",
    "jiiov_reset", "jiiov_irq_work", "jiiov_irq_thread", "jiiov_irq_dispose", "jiiov_irq_free",
    "jiiov_irq_request", "jiiov_irq_enable", "jiiov_resources_release",
    "jiiov_wake_unlock", "jiiov_command", "jiiov_ioctl", "jiiov_open",
    "jiiov_release", "jiiov_prefix", "jiiov_sysfs_command", "resource_set_store", "device_power_store",
    "irq_set_store", "hw_reset_store", "pinctl_set_store", "netlink_event_store",
    "jiiov_stop", "jiiov_remove", "jiiov_shutdown", "jiiov_device_release",
    "jiiov_has_supply_link", "jiiov_validate_supply", "jiiov_parse_dt", "jiiov_probe", "jiiov_init", "jiiov_exit",
)
NETLINK_FUNCTIONS = ("jiiov_netlink_send", "jiiov_netlink_receive", "jiiov_netlink_init", "jiiov_netlink_exit")


def function(source, name):
    # Function signatures start at column zero; never select call sites.
    pattern = r"^(?:static )?(?:[\w]+[ \t*]+)+" + name + r"\([^;{}]*\)\s*\{"
    found = re.search(pattern, source, re.M)
    if not found:
        raise AssertionError(f"missing production function {name}")
    start = found.start()
    cursor = found.end()
    depth = 1
    # Source functions contain no brace characters in literals/comments.
    while depth:
        if cursor == len(source):
            raise AssertionError(f"unterminated function {name}")
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor]


class DriverContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="jiiov-host-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)

    def test_00_source_exists(self):
        for name in ("jiiov_platform.c", "jiiov_netlink.c", "jiiov.h"):
            self.assertTrue((ROOT / name).is_file(), f"missing source driver: {name}")

    def test_10_real_c_contract(self):
        if not (ROOT / "jiiov_platform.c").exists():
            self.skipTest("driver absence reported separately")
        platform = (ROOT / "jiiov_platform.c").read_text()
        netlink = (ROOT / "jiiov_netlink.c").read_text()
        header = (ROOT / "jiiov.h").read_text()
        data = re.search(r"struct jiiov_data \{.*?^\};", header, re.M | re.S)
        self.assertIsNotNone(data, "missing production device state")
        (self.work / "driver.inc").write_text(data[0] + "\n" + "\n\n".join(
            function(platform, name) for name in PLATFORM_FUNCTIONS))
        (self.work / "netlink.inc").write_text("\n\n".join(
            function(netlink, name) for name in NETLINK_FUNCTIONS))
        binary = self.work / "contract"
        command = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                   "-Werror", "-Wno-unused-parameter", "-pthread", "-I", str(self.work),
                   "-I", str(ROOT / "tests"), str(ROOT / "tests/contract.c"),
                   "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main(verbosity=2)
