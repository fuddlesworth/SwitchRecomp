#!/usr/bin/env python3
"""Builds the third-party libraries and tools from the pinned submodules in third_party/, so the repository ships no
prebuilt binaries:

    deps/<target>/lib/libdecoder.a, libcommon.a   OpenH264 decoder (NVDEC video)
    deps/<target>/lib/libspvc.a                   SPIRV-Cross core + GLSL (LSFG shaders)
    deps/<target>/include/{wels,spirv_cross}/     their headers
    deps/host/bin/hactool[.exe]                   extracts game dumps (scripts/extract.py)

    python3 scripts/bootstrap.py                  for this machine (linux, or win64 on Windows)
    python3 scripts/bootstrap.py --target win64   on Linux: cross-built with MinGW-w64 (x86_64-w64-mingw32-gcc)

On Windows, run scripts/bootstrap.ps1 instead: it downloads the toolchain first and then runs this.

Needs a C and C++ compiler, make, cmake, ninja, nasm and git. CC, CXX and AR choose the compiler of a native build.
A library is rebuilt only when its submodule commit or the compiler changed.
"""

import argparse
import io
import os
import shutil
import subprocess
import sys
import tarfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = 'win64' if sys.platform == 'win32' else 'linux'


def run(cmd, log, cwd=None):
    """Runs a build step with its output in log, shown only when the step fails. Third-party code warns a lot."""
    with open(log, 'w') as f:
        r = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        with open(log, errors='replace') as f:
            sys.stderr.write(''.join(f.readlines()[-40:]))
        raise SystemExit('failed: %s (full log: %s)' % (' '.join(cmd), log))


def git(*args):
    return subprocess.run(['git', '-C', REPO, *args], check=True, capture_output=True, text=True).stdout.strip()


class Stamp:
    """Records what a library was built from: its submodule commit and the compiler."""

    def __init__(self, path, submodule, compiler):
        self.path = path
        self.value = '%s %s\n' % (git('-C', os.path.join('third_party', submodule), 'rev-parse', 'HEAD'), compiler)

    def current(self):
        try:
            with open(self.path) as f:
                return f.read() == self.value
        except OSError:
            return False

    def write(self):
        with open(self.path, 'w') as f:
            f.write(self.value)


def export_tree(submodule, dst):
    """A clean copy of a submodule's tree. OpenH264 and hactool build in their source folder, which would leave the
    submodules dirty."""
    shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst)
    data = subprocess.run(
        ['git', '-C', os.path.join(REPO, 'third_party', submodule), 'archive', '--format=tar', 'HEAD'],
        check=True,
        capture_output=True,
    ).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(dst, filter='fully_trusted')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--target', choices=['linux', 'win64'], default=HOST)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    if a.target == HOST:
        cc, cxx, ar = os.environ.get('CC', 'gcc'), os.environ.get('CXX', 'g++'), os.environ.get('AR', 'ar')
        cmake_cross = []
    elif a.target == 'win64' and HOST == 'linux':
        cc, cxx, ar = 'x86_64-w64-mingw32-gcc', 'x86_64-w64-mingw32-g++', 'x86_64-w64-mingw32-ar'
        cmake_cross = ['-DCMAKE_SYSTEM_NAME=Windows']
    else:
        raise SystemExit("can't build for %s on %s" % (a.target, HOST))
    for tool in (cc, cxx, ar, 'make', 'cmake', 'ninja', 'nasm', 'git'):
        if not shutil.which(tool):
            raise SystemExit('%s is not installed' % tool)
    ar_path = shutil.which(ar)

    out = os.path.join(REPO, 'deps', a.target)
    work = os.path.join(REPO, 'build', 'deps', a.target)
    for d in (os.path.join(out, 'lib'), os.path.join(out, 'include'), work):
        os.makedirs(d, exist_ok=True)
    log = os.path.join(work, 'bootstrap.log')
    git('submodule', 'update', '--init', 'third_party/openh264', 'third_party/SPIRV-Cross', 'third_party/hactool')

    # OpenH264: only the decoder and the common code, the runtime never encodes
    stamp = Stamp(os.path.join(out, '.openh264'), 'openh264', cc)
    if stamp.current():
        print('OpenH264 is up to date')
    else:
        print('Building OpenH264 (%s)' % a.target, flush=True)
        src = os.path.join(work, 'openh264')
        export_tree('openh264', src)
        run(
            [
                'make',
                '-C',
                src,
                '-j%d' % a.jobs,
                'OS=' + ('mingw_nt' if a.target == 'win64' else 'linux'),
                'ARCH=x86_64',
                'CC=' + cc,
                'CXX=' + cxx,
                'AR=' + ar,
                'USE_ASM=Yes',
                'BUILDTYPE=Release',
                'libdecoder.a',
                'libcommon.a',
            ],
            log,
        )
        for lib in ('libdecoder.a', 'libcommon.a'):
            shutil.copy(os.path.join(src, lib), os.path.join(out, 'lib'))
        shutil.rmtree(os.path.join(out, 'include', 'wels'), ignore_errors=True)
        shutil.copytree(os.path.join(src, 'codec', 'api', 'wels'), os.path.join(out, 'include', 'wels'))
        shutil.copy(os.path.join(src, 'LICENSE'), os.path.join(out, 'LICENSE-OpenH264.txt'))
        stamp.write()

    # SPIRV-Cross: core and GLSL merged into the one libspvc.a the runtime links
    stamp = Stamp(os.path.join(out, '.spirv-cross'), 'SPIRV-Cross', cxx)
    if stamp.current():
        print('SPIRV-Cross is up to date')
    else:
        print('Building SPIRV-Cross (%s)' % a.target, flush=True)
        bld, inst = os.path.join(work, 'spirv-cross'), os.path.join(work, 'spirv-cross-install')
        shutil.rmtree(bld, ignore_errors=True)
        shutil.rmtree(inst, ignore_errors=True)
        run(
            [
                'cmake',
                '-S',
                os.path.join(REPO, 'third_party', 'SPIRV-Cross'),
                '-B',
                bld,
                '-G',
                'Ninja',
                *cmake_cross,
                '-DCMAKE_C_COMPILER=' + cc,
                '-DCMAKE_CXX_COMPILER=' + cxx,
                '-DCMAKE_AR=' + ar_path,
                '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
                '-DCMAKE_INSTALL_PREFIX=' + inst,
                '-DSPIRV_CROSS_STATIC=ON',
                '-DSPIRV_CROSS_SHARED=OFF',
                '-DSPIRV_CROSS_CLI=OFF',
                '-DSPIRV_CROSS_ENABLE_TESTS=OFF',
                '-DSPIRV_CROSS_ENABLE_HLSL=OFF',
                '-DSPIRV_CROSS_ENABLE_MSL=OFF',
                '-DSPIRV_CROSS_ENABLE_CPP=OFF',
                '-DSPIRV_CROSS_ENABLE_REFLECT=OFF',
                '-DSPIRV_CROSS_ENABLE_C_API=OFF',
                '-DSPIRV_CROSS_ENABLE_UTIL=OFF',
            ],
            log,
        )
        run(['cmake', '--build', bld, '-j%d' % a.jobs], log)
        run(['cmake', '--install', bld], log)
        lib = os.path.join(inst, 'lib')
        # ar's MRI script takes paths relative to the current folder, which keeps Windows drive letters out of it
        mri = 'create libspvc.a\naddlib libspirv-cross-glsl.a\naddlib libspirv-cross-core.a\nsave\nend\n'
        subprocess.run([ar_path, '-M'], input=mri, text=True, cwd=lib, check=True)
        shutil.move(os.path.join(lib, 'libspvc.a'), os.path.join(out, 'lib', 'libspvc.a'))
        shutil.rmtree(os.path.join(out, 'include', 'spirv_cross'), ignore_errors=True)
        shutil.copytree(os.path.join(inst, 'include', 'spirv_cross'), os.path.join(out, 'include', 'spirv_cross'))
        shutil.copy(
            os.path.join(REPO, 'third_party', 'SPIRV-Cross', 'LICENSE'), os.path.join(out, 'LICENSE-SPIRV-Cross.txt')
        )
        stamp.write()

    # hactool runs on the machine doing the extraction, so only a native bootstrap builds it
    host = os.path.join(REPO, 'deps', 'host')
    exe = '.exe' if HOST == 'win64' else ''
    os.makedirs(host, exist_ok=True)
    stamp = Stamp(os.path.join(host, '.hactool'), 'hactool', cc)
    if a.target != HOST:
        print('hactool: built only by a native bootstrap')
    elif stamp.current():
        print('hactool is up to date')
    else:
        print('Building hactool', flush=True)
        src = os.path.join(REPO, 'build', 'deps', 'host', 'hactool')
        export_tree('hactool', src)
        shutil.copy(os.path.join(src, 'config.mk.template'), os.path.join(src, 'config.mk'))
        run(['make', '-C', src, '-j%d' % a.jobs, 'CC=' + cc, 'EXEEXT=' + exe], log)
        os.makedirs(os.path.join(host, 'bin'), exist_ok=True)
        shutil.copy(os.path.join(src, 'hactool' + exe), os.path.join(host, 'bin'))
        shutil.copy(os.path.join(REPO, 'third_party', 'hactool', 'LICENSE'), os.path.join(host, 'LICENSE-hactool.txt'))
        stamp.write()

    print('Dependencies for %s are in deps/%s' % (a.target, a.target))


if __name__ == '__main__':
    sys.exit(main())
