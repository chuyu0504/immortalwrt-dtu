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
        self.sys = self.root / 'sys'
        self.mounts = self.root / 'mounts'
        self.mounts.write_text('tmpfs / tmpfs rw 0 0\n')
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
                                     'IMAGE': str(self.image), 'LOG': str(self.log),
                                     'TEST_SYS': str(self.sys), 'TEST_MOUNTS': str(self.mounts)},
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

    def fixture(self, installed=False, root_lebs=None):
        def write(name, value):
            path = self.sys / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(str(value) + '\n')
        for n in range(3):
            write(f'class/mtd/mtd{n}/flags', 0)
        for key, value in {'size': 526385152, 'writesize': 8192,
                           'erasesize': 524288, 'flags': 1024}.items():
            write('class/mtd/mtd3/' + key, value)
        for key, value in {'ro_mode': 0, 'volumes_count': 9 if installed else 8}.items():
            write('class/ubi/ubi0/' + key, value)
        if root_lebs is None:
            root_lebs = 16 if installed else 920
        volumes = [('mbr', 1), ('boot-resource', 1), ('env', 1), ('env-redund', 1),
                   ('boot', 33), ('rootfs', root_lebs), ('private', 2), ('UDISK', 1)]
        if installed:
            volumes.append(('rootfs_data', 856))
        for n, (name, count) in enumerate(volumes):
            for key, value in {'name': name, 'reserved_ebs': count, 'usable_eb_size': 516096,
                               'upd_marker': 0, 'corrupted': 0}.items():
                write(f'class/ubi/ubi0_{n}/' + key, value)

    def layout_setup(self, extra=''):
        return r'''
cat() {
    case "$1" in /sys/*) command cat "$TEST_SYS${1#/sys}" ;; *) command cat "$@" ;; esac
}
grep() {
    if [ "$3" = /proc/mounts ]; then command grep "$1" "$2" "$TEST_MOUNTS"
    else command grep "$@"; fi
}
find_mtd_index() {
    case "$1" in boot0) echo 0 ;; uboot) echo 1 ;; secure_storage) echo 2 ;; sys) echo 3 ;; esac
}
nand_find_ubi() { echo ubi0; }
nand_find_volume() {
    local f
    for f in "$TEST_SYS"/class/ubi/ubi0_*/name; do
        [ "$(command cat "$f")" = "$2" ] || continue
        f="${f%/name}"; echo "${f##*/}"; return 0
    done
}
rootfs_type() { echo tmpfs; }
get_magic_long() { echo 414e4452; }
nand_remove_ubiblock() { echo "unblock $1" >> "$LOG"; }
ubirmvol() { echo "remove $*" >> "$LOG"; }
ubimkvol() {
    echo "create $*" >> "$LOG"
    if [ "$3" = 8 ]; then touch "$TEST_SYS/ready"; fi
}
''' + extra

    def test_factory_upload_validation_does_not_write(self):
        self.fixture()
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.log.exists())

    def test_installed_layout_still_valid(self):
        self.fixture(installed=True)
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup('rootfs_type() { echo overlayfs; }'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.log.exists())

    def test_factory_migration_requires_ram_root(self):
        self.fixture()
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup('rootfs_type() { echo overlayfs; }'))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.log.exists())

    def test_factory_migration_refuses_unknown_geometry(self):
        self.fixture()
        (self.sys / 'class/mtd/mtd3/writesize').write_text('4096\n')
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertNotEqual(result.returncode, 0)

    def test_factory_migration_refuses_writable_bootloader(self):
        self.fixture()
        (self.sys / 'class/mtd/mtd1/flags').write_text('1024\n')
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertNotEqual(result.returncode, 0)

    def test_factory_migration_refuses_unrecognized_volumes(self):
        self.fixture()
        (self.sys / 'class/ubi/ubi0_6/name').write_text('unknown\n')
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertNotEqual(result.returncode, 0)

    def test_factory_migration_refuses_corrupted_volume(self):
        self.fixture()
        (self.sys / 'class/ubi/ubi0_5/corrupted').write_text('1\n')
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertNotEqual(result.returncode, 0)

    def test_factory_migration_refuses_mounted_rootfs(self):
        self.fixture()
        self.mounts.write_text('ubi0:rootfs /mnt ubifs rw 0 0\n')
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertNotEqual(result.returncode, 0)

    def test_factory_preparation_only_changes_system_volumes(self):
        self.fixture()
        setup = self.layout_setup('ecm30_check_layout() { [ -f "$TEST_SYS/ready" ]; }')
        result = self.run_shell('nand_upgrade_prepare_ubi 512 squashfs 4096', setup)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.log.read_text().splitlines(), [
            'unblock ubi0_5', 'remove /dev/ubi0 -N rootfs',
            'create /dev/ubi0 -n 5 -N rootfs -s 8257536',
            'create /dev/ubi0 -n 8 -N rootfs_data -m', 'unblock ubi0_5'])

    def test_factory_prepare_failure_stops_without_formatting(self):
        self.fixture()
        setup = self.layout_setup('''
ecm30_check_layout() { return 1; }
ubimkvol() { echo "create $*" >> "$LOG"; return 1; }
ubiformat() { echo FORBIDDEN >> "$LOG"; }
''')
        result = self.run_shell('nand_upgrade_prepare_ubi 512 squashfs 4096', setup)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.log.read_text().splitlines(), [
            'unblock ubi0_5', 'remove /dev/ubi0 -N rootfs',
            'create /dev/ubi0 -n 5 -N rootfs -s 8257536'])

    def test_previous_64_block_layout_is_supported_in_ram(self):
        self.fixture(installed=True, root_lebs=64)
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.log.exists())

    def test_previous_layout_cannot_shrink_while_running_from_nand(self):
        self.fixture(installed=True, root_lebs=64)
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup('rootfs_type() { echo overlay; }'))
        self.assertNotEqual(result.returncode, 0)

    def test_previous_layout_refuses_mounted_overlay(self):
        self.fixture(installed=True, root_lebs=64)
        self.mounts.write_text('/dev/ubi0_8 /overlay ubifs rw 0 0\n')
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertNotEqual(result.returncode, 0)

    def test_shrinking_previous_layout_keeps_other_volumes(self):
        self.fixture(installed=True, root_lebs=64)
        setup = self.layout_setup('ecm30_check_layout() { [ -f "$TEST_SYS/ready" ]; }')
        result = self.run_shell('nand_upgrade_prepare_ubi 512 squashfs 4096', setup)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.log.read_text().splitlines(), [
            'unblock ubi0_5', 'remove /dev/ubi0 -N rootfs',
            'remove /dev/ubi0 -N rootfs_data',
            'create /dev/ubi0 -n 5 -N rootfs -s 8257536',
            'create /dev/ubi0 -n 8 -N rootfs_data -m', 'unblock ubi0_5'])

    def test_previous_32_block_layout_is_supported_in_ram(self):
        self.fixture(installed=True, root_lebs=32)
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.log.exists())

    def test_other_rootfs_allocation_is_rejected(self):
        self.fixture(installed=True, root_lebs=48)
        result = self.run_shell('platform_check_image "$IMAGE"', self.layout_setup())
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
