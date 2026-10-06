#!/usr/bin/env python3
"""Run the external wrapper; the leaf records arguments, not a fake build."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class WrapperTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='jiiov-wrapper-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.kernel = self.root / 'kernel'
        self.module = self.root / 'modules/jiiov-fingerprint'
        self.kernel.mkdir()
        self.module.mkdir(parents=True)
        source = Path(__file__).resolve().parents[1]
        for name in ('Makefile', 'Kbuild'):
            shutil.copy2(source / name, self.module / name)
        (self.kernel / 'Makefile').write_text(
            '.PHONY: modules modules_install clean\n'
            'modules modules_install clean:\n'
            '\t@printf "%s\\n" "target=$@" "M=$(M)" "O=$(O)" '
            '"ARCH=$(ARCH)" "LLVM=$(LLVM)" "CC=$(CC)" '
            '"INSTALL_MOD_PATH=$(INSTALL_MOD_PATH)" "INSTALL_MOD_STRIP=$(INSTALL_MOD_STRIP)"\n'
            '\t@test "$(FAIL_TARGET)" != "$@"\n')

    def invoke(self, *args):
        return subprocess.run(['make', '--no-print-directory', '-C', str(self.module),
                               'KERNEL_SRC=' + str(self.kernel),
                               'O=' + str(self.root / 'out/KERNEL_OBJ'),
                               'ARCH=arm64', 'LLVM=1', 'CC=rom-clang', *args],
                              text=True, capture_output=True)

    def test_default_and_explicit_targets_preserve_relative_and_absolute_m(self):
        for module in ('../modules/jiiov-fingerprint', str(self.module)):
            for target in ('', 'modules', 'modules_install', 'clean'):
                with self.subTest(module=module, target=target):
                    args = ['M=' + module, 'INSTALL_MOD_PATH=' + str(self.root / 'install')]
                    if target:
                        args.append(target)
                    result = self.invoke(*args)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    values = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
                    self.assertEqual(values['target'], target or 'modules')
                    self.assertEqual(values['M'], module)
                    self.assertEqual(values['O'], str(self.root / 'out/KERNEL_OBJ'))
                    self.assertEqual((values['ARCH'], values['LLVM'], values['CC']),
                                     ('arm64', '1', 'rom-clang'))
                    if target == 'modules_install':
                        self.assertEqual(values['INSTALL_MOD_PATH'], str(self.root / 'install'))
                        self.assertEqual(values['INSTALL_MOD_STRIP'], '1')

    def test_no_m_uses_current_module_directory(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('M=' + str(self.module), result.stdout.splitlines())

    def test_leaf_failure_is_not_hidden(self):
        for target in ('modules', 'modules_install'):
            with self.subTest(target=target):
                result = self.invoke('M=../modules/jiiov-fingerprint', 'FAIL_TARGET=' + target, target)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('target=' + target, result.stdout.splitlines())


if __name__ == '__main__':
    unittest.main()
