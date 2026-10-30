#!/usr/bin/env python3
"""Build a pinned Rockchip plugin into the project; never install system-wide."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

PROJECT = Path(__file__).resolve().parents[1]


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def download(item, destination):
    if destination.is_file() and sha256(destination) == item['sha256']:
        return
    temporary = destination.with_suffix(destination.suffix + '.part')
    try:
        request = urllib.request.Request(item['url'], headers={'User-Agent': 'AI-Camera-plugin-build'})
        with urllib.request.urlopen(request, timeout=60) as source, temporary.open('wb') as output:
            shutil.copyfileobj(source, output)
        if sha256(temporary) != item['sha256']:
            raise RuntimeError('Source archive SHA-256 does not match the pinned manifest')
        temporary.replace(destination)
    finally:
        if temporary.exists():
            temporary.unlink()


def extract(archive, destination):
    # Both pinned archives contain one enclosing directory. Links and special
    # files are rejected, and extraction never writes outside its destination.
    if destination.exists():
        return
    temporary = destination.with_name(destination.name + '.extracting')
    if temporary.exists():
        raise RuntimeError(f'Incomplete source extraction: {temporary}; inspect it before retrying')
    temporary.mkdir()
    with tarfile.open(archive) as bundle:
        for member in bundle:
            parts = Path(member.name).parts
            if not parts or Path(member.name).is_absolute() or '..' in parts:
                raise RuntimeError('Unsafe source archive path')
            relative = Path(*parts[1:])
            if not relative.parts:
                continue
            target = (temporary / relative).resolve()
            if not target.is_relative_to(temporary.resolve()) or not (member.isdir() or member.isfile()):
                raise RuntimeError('Unsupported source archive member')
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with bundle.extractfile(member) as source, target.open('wb') as output:
                    shutil.copyfileobj(source, output)
    temporary.replace(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=PROJECT / 'build_gst')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() not in ('aarch64', 'arm64'):
        raise RuntimeError('Build this plugin on the Linux ARM64 Firefly host')
    for command in ['cc', 'pkg-config', 'ninja', 'gst-inspect-1.0']:
        if not shutil.which(command):
            raise RuntimeError(f'Missing build dependency: {command}')
    lock_path = Path(__file__).with_name('rockchip_plugin.lock.json')
    lock = json.loads(lock_path.read_text())
    root = args.output.expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    import fcntl
    with (root / 'build.lock').open('a') as build_lock:
        fcntl.flock(build_lock, fcntl.LOCK_EX)
        prefix = root / 'prefix'
        plugin = prefix / 'lib/gstreamer-1.0/libgstrockchipmpp.so'
        manifest_path = root / 'manifest.json'
        lock_hash = sha256(lock_path)
        builder_hash = sha256(Path(__file__).resolve())
        dependencies = subprocess.check_output(['pkg-config', '--modversion',
            'gstreamer-1.0', 'gstreamer-video-1.0', 'rockchip_mpp'], text=True).splitlines()
        if manifest_path.is_file() and plugin.is_file():
            old = json.loads(manifest_path.read_text())
            if (old.get('lock_sha256') == lock_hash and old.get('builder_sha256') == builder_hash
                    and old.get('dependencies') == dependencies
                    and old.get('plugin_sha256') == sha256(plugin)):
                print(f'[MPP BUILD] verified existing plugin: {plugin}')
                return
        cache = root / 'downloads'
        cache.mkdir(exist_ok=True)
        source_archive = cache / (lock['plugin']['commit'] + '.tar.gz')
        meson_archive = cache / ('meson-' + lock['meson']['version'] + '.tar.gz')
        download(lock['plugin'], source_archive)
        download(lock['meson'], meson_archive)
        source = root / ('source-' + lock['plugin']['commit'])
        meson_source = root / ('meson-' + lock['meson']['version'])
        extract(source_archive, source)
        extract(meson_archive, meson_source)
        # Upstream's build copies its development hook here even for archives.
        (source / '.git/hooks').mkdir(parents=True, exist_ok=True)
        decoder_source = source / 'gst/rockchipmpp/gstmppdec.c'
        if 'mpp_frame_deinit (&self->mpp_frame)' not in decoder_source.read_text():
            raise RuntimeError('Pinned decoder source is missing the corrected deinit call')
        build = root / 'build'
        setup = [sys.executable, str(meson_source / 'meson.py'), 'setup']
        if (build / 'meson-private/coredata.dat').exists():
            setup.append('--reconfigure')
        setup += [str(build), str(source), '--prefix', str(prefix), '--libdir', 'lib',
                  '--buildtype', 'release', '-Drockchipmpp=enabled', '-Drkximage=disabled',
                  '-Dkmssrc=disabled', '-Dvpxalphadec=disabled', '-Drga=disabled']
        subprocess.run(setup, check=True)
        subprocess.run(['ninja', '-C', str(build)], check=True)
        subprocess.run(['ninja', '-C', str(build), 'install'], check=True)
        env = dict(os.environ, GST_PLUGIN_PATH_1_0=str(plugin.parent),
                   GST_REGISTRY_1_0=str(root / 'registry.bin'))
        info = subprocess.check_output(['gst-inspect-1.0', 'mppvideodec'], env=env, text=True)
        filename = next(line.split(None, 1)[1] for line in info.splitlines()
                        if line.strip().startswith('Filename '))
        if Path(filename).resolve() != plugin.resolve():
            raise RuntimeError('GStreamer selected a different decoder plugin')
        manifest = {'source': lock['plugin'], 'meson': lock['meson'], 'lock_sha256': lock_hash,
                    'builder_sha256': builder_hash,
                    'plugin_sha256': sha256(plugin), 'plugin': str(plugin),
                    'dependencies': dependencies}
        temporary = manifest_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(manifest, indent=2) + '\n')
        temporary.replace(manifest_path)
        print(f'[MPP BUILD] ready: {plugin}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'[MPP BUILD ERROR] {error}', file=sys.stderr)
        sys.exit(1)
