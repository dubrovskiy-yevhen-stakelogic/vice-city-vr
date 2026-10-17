#!/usr/bin/env python3
"""Vice City VR required runtime setup for Linux (Steam Play / Proton).

Linux counterpart of SETUP_RUNTIME.bat / install-dlss5.ps1 -Profile BASE.
It reads the same pinned catalog (packages.json), downloads the same files,
verifies size and SHA-256 of every archive and payload, backs up replaced
files under dlss5-backups/ and installs them beside reVC.exe.

Only the BASE profile is offered. Neural DLSS 5 has not been validated under
Proton; use the Windows installer for the NR profiles.

Standard library only. No root rights, Wine prefix or PowerShell are needed.
"""

import argparse
import datetime
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROFILE = 'BASE'
BASE_NAMES = ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll',
              'sl.pcl.dll', 'nvngx_dlss.dll')
ALL_NAMES = BASE_NAMES + ('sl.dlss_nr.dll', 'nvngx_dlssnr.dll')
DOWNLOAD_HOSTS = ('github.com', 'raw.githubusercontent.com')
REDIRECT_HOSTS = DOWNLOAD_HOSTS + (
    'release-assets.githubusercontent.com', 'objects.githubusercontent.com',
    'media.githubusercontent.com')
MAX_PACKAGE_SIZE = 250000000
NVIDIA_TERMS = ('https://github.com/NVIDIA/DLSS/blob/'
                'a291cc7d2cc642a51566f3dfd5376f635cd1b284/LICENSE.txt')


class SetupError(Exception):
    pass


def kit_version():
    try:
        with open(os.path.join(SCRIPT_DIR, '..', '..', 'VERSION'), encoding='utf-8') as handle:
            return handle.read().strip() or 'unknown'
    except OSError:
        return 'unknown'


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1048576), b''):
            digest.update(block)
    return digest.hexdigest()


def is_sha256(value):
    return (isinstance(value, str) and len(value) == 64 and
            all(c in '0123456789abcdef' for c in value))


def resolve_dir(path):
    # Linked parents are normal on Linux (~/.steam/steam, /home on Fedora
    # Atomic), so folders are resolved instead of rejected.
    return os.path.realpath(os.path.expanduser(path))


def assert_not_link(path):
    """Never read or replace a runtime file through a symlink."""
    full = os.path.abspath(path)
    if os.path.islink(full):
        raise SetupError('Expected a normal file or folder, found a link: ' + full)
    return full


def assert_pe64(path):
    with open(path, 'rb') as handle:
        data = handle.read(4096)
    size = os.path.getsize(path)
    if size < 128 or data[:2] != b'MZ':
        raise SetupError('Not a Windows executable: ' + path)
    offset = struct.unpack_from('<i', data, 0x3c)[0]
    if offset < 64 or offset > size - 26 or offset + 26 > len(data):
        raise SetupError('Invalid executable header: ' + path)
    signature, machine = struct.unpack_from('<IH', data, offset)
    if signature != 0x4550 or machine != 0x8664:
        raise SetupError('A Windows x64 file is required: ' + path)
    if struct.unpack_from('<H', data, offset + 24)[0] != 0x20b:
        raise SetupError('Not a PE32+ executable: ' + path)


def assert_game_closed():
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            with open('/proc/%s/cmdline' % pid, 'rb') as handle:
                arguments = handle.read().split(b'\0')
        except OSError:
            continue
        for argument in arguments[:2]:
            name = argument.replace(b'\\', b'/').rsplit(b'/', 1)[-1].lower()
            if name == b'revc.exe':
                raise SetupError('Close Vice City VR before installing or restoring '
                                 'files. Setup will not stop the game for you.')


def get_game_directory(game_dir):
    root = resolve_dir(game_dir)
    if os.path.isfile(root) and os.path.basename(root).lower() == 'revc.exe':
        root = os.path.dirname(root)
    if not os.path.isdir(root):
        raise SetupError('Game folder does not exist: ' + root)
    if os.path.exists(os.path.join(root, 'patch-manifest.json')):
        for relative in ('data/gta_vc.dat', 'models/gta3.img'):
            asset = os.path.join(root, relative)
            if not os.path.isfile(asset) or os.path.getsize(asset) == 0:
                raise SetupError('This folder contains patch-only source kit metadata '
                                 'but is missing game data: %s. Select the folder of '
                                 'your complete Vice City VR game installation.' % relative)
    for name in ('reVC.exe', 'nvngx.dll_dlssnr.dll'):
        path = assert_not_link(os.path.join(root, name))
        if not os.path.isfile(path):
            raise SetupError('Missing %s. Select the folder containing the complete '
                             'Vice City VR player build. This installer does not '
                             'build the game.' % name)
        assert_pe64(path)
    return root


def load_packages():
    with open(os.path.join(SCRIPT_DIR, 'packages.json'), encoding='utf-8') as handle:
        catalog = json.load(handle)
    if catalog.get('schema') != 1:
        raise SetupError('Unsupported download catalog.')
    streamline = dict(catalog['streamline'])
    streamline['files'] = [f for f in streamline['files'] if f['name'] in BASE_NAMES]
    packages = [streamline, catalog['superResolution']]
    names = [f['name'] for p in packages for f in p['files']]
    if sorted(names) != sorted(BASE_NAMES):
        raise SetupError('A complete, unique runtime set is required for profile BASE.')
    return packages


def assert_payload(path, spec):
    if spec['name'] not in ALL_NAMES:
        raise SetupError('Unexpected runtime filename.')
    assert_not_link(path)
    if (not os.path.isfile(path) or os.path.getsize(path) != int(spec['size']) or
            sha256_file(path) != spec['sha256']):
        raise SetupError('File verification failed: %s. Nothing from this download '
                         'will be installed.' % spec['name'])
    assert_pe64(path)
    # Authenticode cannot be validated with the standard Linux toolset. The
    # pinned SHA-256 binds the exact NVIDIA-signed bytes the Windows installer
    # accepts, so a matching hash implies the same signed file.
    if spec.get('signature') not in ('NVIDIA', 'ModifiedPinned'):
        raise SetupError('Unknown signature policy.')


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def get_download(package, cache_dir):
    url = urllib.parse.urlsplit(package['url'])
    if (url.scheme != 'https' or url.hostname not in DOWNLOAD_HOSTS or
            not is_sha256(package['sha256']) or
            not 0 < int(package['size']) <= MAX_PACKAGE_SIZE):
        raise SetupError('Invalid pinned download entry.')
    os.makedirs(cache_dir, exist_ok=True)
    path = assert_not_link(os.path.join(cache_dir, package['sha256'] + '.download'))
    if os.path.isfile(path):
        if (os.path.getsize(path) == int(package['size']) and
                sha256_file(path) == package['sha256']):
            return path
        raise SetupError('A cached download failed verification. Remove this one '
                         'file and retry: ' + path)
    partial = os.path.join(cache_dir, uuid.uuid4().hex + '.partial')
    print('Downloading %s (%.1f MB)...' % (package['label'], int(package['size']) / 1048576))
    opener = urllib.request.build_opener(_NoRedirect)
    current = package['url']
    try:
        for redirect in range(7):
            parts = urllib.parse.urlsplit(current)
            if parts.scheme != 'https' or parts.hostname not in REDIRECT_HOSTS:
                raise SetupError('Download redirected outside the permitted HTTPS hosts.')
            request = urllib.request.Request(
                current, headers={'User-Agent': 'ViceCityVR-Setup/' + kit_version()})
            try:
                response = opener.open(request, timeout=30)
            except urllib.error.HTTPError as error:
                if error.code in (301, 302, 303, 307, 308):
                    location = error.headers.get('Location')
                    if not location or redirect == 6:
                        raise SetupError('Invalid download redirect.')
                    current = urllib.parse.urljoin(current, location)
                    continue
                raise SetupError('Download failed: HTTP %d' % error.code)
            break
        with response:
            length = response.headers.get('Content-Length')
            if length and int(length) != int(package['size']):
                raise SetupError('The download size differs from the pinned release.')
            total = 0
            deadline = datetime.datetime.now() + datetime.timedelta(minutes=10)
            with open(partial, 'xb') as output:
                while True:
                    block = response.read(1048576)
                    if not block:
                        break
                    if datetime.datetime.now() > deadline:
                        raise SetupError('Download timed out. Please retry.')
                    total += len(block)
                    if total > int(package['size']):
                        raise SetupError('Download exceeded the pinned size.')
                    output.write(block)
                    sys.stdout.write('\r  %.1f / %.1f MB' % (
                        total / 1048576, int(package['size']) / 1048576))
                    sys.stdout.flush()
            print()
        if total != int(package['size']) or sha256_file(partial) != package['sha256']:
            raise SetupError('Download checksum mismatch. It may be damaged or the '
                             'remote asset may have changed.')
        os.rename(partial, path)
        return path
    except (urllib.error.URLError, OSError) as error:
        raise SetupError('Download failed: %s' % error)
    finally:
        if os.path.isfile(partial):
            os.remove(partial)


def expand_package(archive_path, package, stage):
    if package['format'] == 'dll':
        if len(package['files']) != 1:
            raise SetupError('Invalid raw DLL package.')
        spec = package['files'][0]
        if spec['name'] not in ALL_NAMES:
            raise SetupError('Unexpected runtime filename.')
        target = os.path.join(stage, spec['name'])
        shutil.copyfile(archive_path, target)
        assert_payload(target, spec)
        return
    if package['format'] != 'zip':
        raise SetupError('Unsupported package format.')
    with zipfile.ZipFile(archive_path) as archive:
        entries = archive.infolist()
        for entry in entries:
            relative = entry.filename.replace('\\', '/')
            if (relative.startswith('/') or ':' in relative or
                    any(part in ('.', '..') for part in relative.split('/'))):
                raise SetupError('Unsafe archive entry.')
        for spec in package['files']:
            if spec['name'] not in ALL_NAMES:
                raise SetupError('Unexpected runtime filename.')
            matches = [e for e in entries if e.filename == spec['archivePath']]
            if len(matches) != 1 or matches[0].file_size != int(spec['size']):
                raise SetupError('Archive contents differ from the pinned package: ' +
                                 spec['name'])
            target = os.path.join(stage, spec['name'])
            with archive.open(matches[0]) as source, open(target, 'xb') as output:
                shutil.copyfileobj(source, output)
            assert_payload(target, spec)


def set_file_atomic(source, target, expected_sha256):
    assert_not_link(target)
    temporary = os.path.join(os.path.dirname(target),
                             '.dlss5-' + uuid.uuid4().hex + '.tmp')
    try:
        shutil.copyfile(source, temporary)
        if not is_sha256(expected_sha256) or sha256_file(temporary) != expected_sha256:
            raise SetupError('The replacement file changed or its temporary copy '
                             'failed verification.')
        os.replace(temporary, target)
    finally:
        if os.path.isfile(temporary):
            os.remove(temporary)


def install_files(game, stage, files):
    assert_game_closed()
    changes = []
    for spec in files:
        assert_payload(os.path.join(stage, spec['name']), spec)
        target = assert_not_link(os.path.join(game, spec['name']))
        if os.path.exists(target) and not os.path.isfile(target):
            raise SetupError('Expected a file, found a directory: ' + target)
        existed = os.path.isfile(target)
        old_hash = sha256_file(target) if existed else ''
        if old_hash == spec['sha256']:
            continue
        changes.append({'name': spec['name'], 'existed': existed,
                        'previousSha256': old_hash, 'installedSha256': spec['sha256']})
    if not changes:
        print('This exact runtime set is already installed.')
        return None
    backup_root = assert_not_link(os.path.join(game, 'dlss5-backups'))
    backup = os.path.join(backup_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S') +
                          '-' + uuid.uuid4().hex[:8])
    os.makedirs(backup)
    for entry in changes:
        if entry['existed']:
            saved = os.path.join(backup, entry['name'])
            shutil.copyfile(os.path.join(game, entry['name']), saved)
            if sha256_file(saved) != entry['previousSha256']:
                raise SetupError('Runtime changed while backing up. No runtime files '
                                 'were replaced.')
    journal = {'schema': 1, 'gameDir': game, 'profile': PROFILE, 'files': changes}
    with open(os.path.join(backup, 'manifest.json'), 'w', encoding='utf-8') as handle:
        json.dump(journal, handle, indent=4)
    changed = []
    try:
        for entry in changes:
            assert_game_closed()
            target = assert_not_link(os.path.join(game, entry['name']))
            current = sha256_file(target) if os.path.isfile(target) else ''
            if current != entry['previousSha256']:
                raise SetupError('A runtime file changed during installation.')
            set_file_atomic(os.path.join(stage, entry['name']), target,
                            entry['installedSha256'])
            changed.append(entry)
            if sha256_file(target) != entry['installedSha256']:
                raise SetupError('Installed file verification failed.')
    except (SetupError, OSError) as error:
        for entry in changed:
            target = os.path.join(game, entry['name'])
            try:
                if sha256_file(target) != entry['installedSha256']:
                    raise SetupError('The file changed again; automatic restore refused.')
                if entry['existed']:
                    set_file_atomic(os.path.join(backup, entry['name']), target,
                                    entry['previousSha256'])
                else:
                    os.remove(target)
            except (SetupError, OSError) as restore_error:
                print('WARNING: Could not restore %s: %s. Backup: %s' % (
                    entry['name'], restore_error, backup))
        raise SetupError('Installation failed: %s. Original files are preserved in %s' % (
            error, backup))
    print('Backup: ' + backup)
    return backup


def restore_backup(game, backup_dir):
    assert_game_closed()
    backup = resolve_dir(backup_dir)
    if os.path.dirname(backup) != os.path.join(game, 'dlss5-backups'):
        raise SetupError('Select a backup created inside this game folder.')
    with open(os.path.join(backup, 'manifest.json'), encoding='utf-8') as handle:
        journal = json.load(handle)
    if journal.get('schema') != 1 or journal.get('gameDir') != game:
        raise SetupError('This backup belongs to a different installation.')
    allowed = BASE_NAMES if journal.get('profile') == 'BASE' else ALL_NAMES
    seen = set()
    pending = []
    for entry in journal.get('files', []):
        if (entry.get('name') not in allowed or entry['name'] in seen or
                not is_sha256(entry.get('installedSha256')) or
                not isinstance(entry.get('existed'), bool)):
            raise SetupError('Invalid backup manifest.')
        seen.add(entry['name'])
        target = assert_not_link(os.path.join(game, entry['name']))
        current = sha256_file(target) if os.path.isfile(target) else ''
        if entry['existed']:
            source = os.path.join(backup, entry['name'])
            if (not is_sha256(entry.get('previousSha256')) or not os.path.isfile(source) or
                    sha256_file(source) != entry['previousSha256']):
                raise SetupError('Backup data is missing or changed.')
            if current == entry['previousSha256']:
                continue
        elif not current:
            continue
        if current != entry['installedSha256']:
            raise SetupError('Changed since installation: %s. Restore stopped to '
                             'preserve your newer files.' % entry['name'])
        pending.append(entry)
    for entry in pending:
        assert_game_closed()
        target = os.path.join(game, entry['name'])
        if sha256_file(target) != entry['installedSha256']:
            raise SetupError('A runtime file changed during restore.')
        if entry['existed']:
            set_file_atomic(os.path.join(backup, entry['name']), target,
                            entry['previousSha256'])
        else:
            os.remove(target)
    print('Previous runtime restored. Backup copies have been kept.')


def choose_backup(game):
    root = os.path.join(game, 'dlss5-backups')
    backups = sorted((d for d in os.listdir(root) if os.path.isfile(
        os.path.join(root, d, 'manifest.json'))), reverse=True) if os.path.isdir(root) else []
    if not backups:
        raise SetupError('No installer backups were found in this game folder.')
    for number, name in enumerate(backups, 1):
        print('%d - %s' % (number, name))
    answer = input('Select a backup number: ').strip()
    if not answer.isdigit() or not 1 <= int(answer) <= len(backups):
        raise SetupError('Cancelled: no valid backup selected.')
    return os.path.join(root, backups[int(answer) - 1])


def default_cache_dir():
    base = os.environ.get('XDG_CACHE_HOME') or os.path.join(os.path.expanduser('~'), '.cache')
    return os.path.join(base, 'ViceCityVR', 'DLSS5Cache')


def print_proton_notes():
    print('')
    print('Start reVC.exe as a non-Steam game with Proton Experimental while your')
    print('OpenXR runtime (for example SteamVR) is running. Recommended launch options:')
    print('    PROTON_ENABLE_NVAPI=1 %command%')
    print('See docs/LINUX.md for the full Linux guide and troubleshooting.')


def main():
    parser = argparse.ArgumentParser(
        description='Install the required Vice City VR runtime (BASE profile) on Linux.')
    parser.add_argument('--game-dir', help='folder containing reVC.exe (or reVC.exe itself)')
    parser.add_argument('--cache-dir', help='download cache (default: XDG cache folder)')
    parser.add_argument('--restore', nargs='?', const='', metavar='BACKUP',
                        help='restore a previous runtime backup (choose interactively '
                             'when no folder is given)')
    parser.add_argument('--verify-only', action='store_true',
                        help='download and verify only; do not change the game folder')
    parser.add_argument('--no-prompt', action='store_true')
    parser.add_argument('--accept-community-runtime', action='store_true',
                        help='accept the third-party Streamline mirror without a prompt')
    args = parser.parse_args()

    stage = None
    try:
        if args.verify_only and args.restore is not None:
            raise SetupError('--verify-only cannot be combined with --restore.')
        print('Vice City VR %s - Required Runtime Setup (Linux)' % kit_version())
        print('No compiler, Git, root rights or VR headset is needed for setup.')
        print('The game is never launched and VR/settings files are not changed.')
        print('')
        restore = args.restore is not None
        if not args.no_prompt and not args.verify_only and not restore:
            print('1 - Install the required runtime (BASE)')
            print('2 - Restore a previous runtime backup')
            print('Anything else - Cancel')
            choice = input('Choose 1 or 2: ').strip()
            if choice == '2':
                restore = True
            elif choice != '1':
                print('Cancelled.')
                return 0
        game = None
        if restore or not args.verify_only:
            game_dir = args.game_dir
            if not game_dir:
                if args.no_prompt:
                    raise SetupError('--game-dir is required without prompts.')
                game_dir = input('Path to your Vice City VR folder (containing reVC.exe): ').strip()
                if not game_dir:
                    raise SetupError('Cancelled. No runtime files were changed.')
            game = get_game_directory(game_dir)
            assert_game_closed()
            print('Game folder: ' + game)
        if restore:
            backup = args.restore or choose_backup(game)
            if not args.no_prompt and input('Type RESTORE to restore the selected '
                                            'backup: ').strip() != 'RESTORE':
                print('Cancelled.')
                return 0
            restore_backup(game, backup)
            return 0

        packages = load_packages()
        files = [f for p in packages for f in p['files']]
        print('')
        print('Sources and pinned versions:')
        for package in packages:
            print(package['label'])
            print(package['url'])
        print('')
        print('DLSS SR is downloaded directly from NVIDIA. The required Streamline runtime '
              'uses RankFTW/rhi-repo, a third-party mirror.')
        print('BASE installs five runtime files. No NR model is downloaded; existing '
              'DLSS 5 plugin/model files are left untouched.')
        print('Checksums pin these exact files; they do not guarantee safety, licensing '
              'permission or in-game compatibility.')
        print('Review the NVIDIA RTX SDK terms and the linked releases before accepting.')
        print(NVIDIA_TERMS)
        if not args.accept_community_runtime:
            if args.no_prompt:
                raise SetupError('Explicit --accept-community-runtime is required for '
                                 'third-party downloads.')
            if input('Type INSTALL to accept these sources and continue: ').strip() != 'INSTALL':
                print('Cancelled.')
                return 0

        cache = resolve_dir(args.cache_dir or default_cache_dir())
        kit_root = os.path.realpath(os.path.join(SCRIPT_DIR, '..', '..'))
        if cache == kit_root or cache.startswith(kit_root + os.sep):
            raise SetupError('Download caches must stay outside the source kit. Omit '
                             '--cache-dir to use the normal cache folder.')
        os.makedirs(cache, exist_ok=True)
        stage = tempfile.mkdtemp(prefix='stage-', dir=cache)
        for package in packages:
            expand_package(get_download(package, cache), package, stage)
        if args.verify_only:
            print('VERIFIED: all %d pinned runtime files passed the selected checks. '
                  'No game files were changed.' % len(files))
        else:
            install_files(game, stage, files)
            print('INSTALLED: runtime files verified and placed beside reVC.exe.')
            print('DLSS 5 is optional and was not installed. Neural rendering has not '
                  'been validated under Proton.')
            print('To undo this installation, run SETUP_RUNTIME_LINUX.sh again and '
                  'choose Restore.')
            print_proton_notes()
        print('Verified downloads are cached for reuse: ' + cache)
        return 0
    except (SetupError, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print('')
        print('SETUP STOPPED: %s' % error)
        print('Keep the full setup output when reporting this error. Do not disable '
              'file verification.')
        return 1
    except (KeyboardInterrupt, EOFError):
        print('')
        print('Cancelled.')
        return 1
    finally:
        if stage and os.path.isdir(stage):
            shutil.rmtree(stage, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
