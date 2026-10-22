#!/usr/bin/env python3
"""Exercise ECM30 upgrade rejection and write ordering without block devices."""
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
PLATFORM = REPO / 'target/linux/sunxi/base-files/lib/upgrade/platform.sh'
DIR = 'sysupgrade-ebyte_ecm30'


class UpgradeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / 'upgrade.tar'
        self.log = self.root / 'operations'
        self.archive()

    def archive(self, root=b'hsqs' + bytes(508), extra=None, duplicate=False):
        with tarfile.open(self.image, 'w') as archive:
            files = [(DIR + '/CONTROL', b'BOARD=ebyte_ecm30\n'),
                     (DIR + '/kernel', b'ANDROID!' + bytes(4088)),
                     (DIR + '/root', root)]
            if extra:
                files.append(extra)
            if duplicate:
                files.append(files[-1])
            for name, data in files:
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))

    def run_shell(self, script, setup=''):
        prologue = r'''
board_name() { echo ebyte,ecm30; }
get_magic_long_tar() {
    tar xOf "$1" "$3" | dd bs=4 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n'
}
. "$PLATFORM"
'''
        result = subprocess.run(['sh', '-c', prologue + setup + '\n' + script],
                                env={'PATH': '/usr/bin:/bin', 'PLATFORM': str(PLATFORM),
                                     'IMAGE': str(self.image), 'LOG': str(self.log)},
                                capture_output=True, text=True)
        return result

    def test_valid_archive(self):
        self.assertEqual(self.run_shell('ecm30_check_tar "$IMAGE"').returncode, 0)

    def test_wrong_rootfs_magic(self):
        self.archive(root=b'UBI#' + bytes(508))
        self.assertNotEqual(self.run_shell('ecm30_check_tar "$IMAGE"').returncode, 0)

    def test_extra_member(self):
        self.archive(extra=('../boot0', bytes(4096)))
        self.assertNotEqual(self.run_shell('ecm30_check_tar "$IMAGE"').returncode, 0)

    def test_duplicate_member(self):
        self.archive(duplicate=True)
        self.assertNotEqual(self.run_shell('ecm30_check_tar "$IMAGE"').returncode, 0)

    def test_wrong_board(self):
        self.assertNotEqual(self.run_shell('ecm30_check_tar "$IMAGE"',
                            'board_name() { echo another,board; }').returncode, 0)

    def test_capacity_limit(self):
        self.assertNotEqual(self.run_shell('ecm30_check_tar "$IMAGE"',
                            'ECM30_BOOT_BYTES=4095').returncode, 0)

    def test_invalid_archive_never_writes(self):
        self.archive(root=b'UBI#' + bytes(508))
        setup = '''
ecm30_check_layout() { return 0; }
nand_remove_ubiblock() { echo WRITE >> "$LOG"; }
nand_upgrade_tar() { echo WRITE >> "$LOG"; }
ubirmvol() { echo WRITE >> "$LOG"; }
'''
        self.assertNotEqual(self.run_shell('platform_do_upgrade "$IMAGE"', setup).returncode, 0)
        self.assertFalse(self.log.exists())

    def flow_setup(self, failure=''):
        return '''
ecm30_check_layout() { return 0; }
nand_remove_ubiblock() { echo "unblock $1" >> "$LOG"; }
nand_upgrade_tar() {
    nand_remove_ubiblock ubi0_5
    echo 'write ubi0_5' >> "$LOG"
    [ 'ubi0_5' != 'FAIL_VOLUME' ] || return 1
    echo 'write ubi0_4' >> "$LOG"
    [ 'ubi0_4' != 'FAIL_VOLUME' ] || return 1
}
ecm30_verify_volume() {
    return 0
}
ubirmvol() { echo "remove $*" >> "$LOG"; }
ubimkvol() { echo "create $*" >> "$LOG"; }
nand_do_upgrade_success() { echo success >> "$LOG"; }
'''.replace('FAIL_VOLUME', failure)

    def test_upgrade_only_changes_expected_volumes(self):
        result = self.run_shell('platform_do_upgrade "$IMAGE"', self.flow_setup())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.log.read_text().splitlines(), [
            'unblock ubi0_5', 'write ubi0_5', 'write ubi0_4',
            'remove /dev/ubi0 -N rootfs_data',
            'create /dev/ubi0 -n 8 -N rootfs_data -m', 'success'])

    def test_root_write_failure_keeps_kernel_and_overlay(self):
        result = self.run_shell('platform_do_upgrade "$IMAGE"', self.flow_setup('ubi0_5'))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.log.read_text().splitlines(), ['unblock ubi0_5', 'write ubi0_5'])

    def test_kernel_failure_keeps_overlay(self):
        result = self.run_shell('platform_do_upgrade "$IMAGE"', self.flow_setup('ubi0_4'))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.log.read_text().splitlines(),
                         ['unblock ubi0_5', 'write ubi0_5', 'write ubi0_4'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
