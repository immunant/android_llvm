#!/usr/bin/env python
#
# Copyright (C) 2016 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

#from __future__ import print_function

import argparse
import glob
import multiprocessing
import os
import shutil
import subprocess
import sys

THIS_DIR = os.path.realpath(os.path.dirname(__file__))
ORIG_ENV = dict(os.environ)


def android_path(*args):
    return os.path.realpath(os.path.join(THIS_DIR, '../../', *args))


#import version
# TODO(pirama): Automatically detect clang version
def clang_version():
    return '5.0.0'


def ndk_path():
    # TODO Switch to r13 from the toolchain/prebuilts/ndk/r13 branch
    ndk_version = 'r10'
    platform_level = 'android-23'
    return android_path('prebuilts/ndk', ndk_version, 'platforms', platform_level)


def build_os_type():
    if sys.platform.startswith('linux'):
        return 'linux-x86'
    else:
        return 'darwin-x86'


def cmake_prebuilt_bin_dir():
    return android_path('prebuilts/cmake', build_os_type(), 'bin')


def cmake_bin_path():
    return os.path.join(cmake_prebuilt_bin_dir(), 'cmake')


def ninja_bin_path():
    return os.path.join(cmake_prebuilt_bin_dir(), 'ninja')


def check_create_path(path):
    if not os.path.exists(path):
        os.makedirs(path)


def rm_tree(dir):
    def chmod_and_retry(func, path, _):
        if not os.access(path, os.W_OK):
            os.chmod(path, stat.S_IWUSR)
            return func(path)
        raise
    shutil.rmtree(dir, onerror=chmod_and_retry)


def rm_cmake_cache(dir):
    for dirpath, dirs, files in os.walk(dir):
        if 'CMakeCache.txt' in files:
            os.remove(os.path.join(dirpath, 'CMakeCache.txt'))
        if 'CMakeFiles' in dirs:
            rm_tree(os.path.join(dirpath, 'CMakeFiles'))


def build_llvm(out_path, defines, env, cmake_path, target=None):
    flags = ['-G', 'Ninja']

    # Specify CMAKE_PREFIX_PATH so 'cmake -G Ninja ...' can find the ninja
    # executable.
    flags += ['-DCMAKE_PREFIX_PATH=' + cmake_prebuilt_bin_dir()]

    for key in defines:
        newdef = '-D' + key + '=' + defines[key]
        flags += [newdef]
    flags += [cmake_path]

    check_create_path(out_path)
    os.chdir(out_path)
    # TODO(srhines): Enable this with a flag, because it forces clean builds
    # due to the updated cmake generated files.
    #rm_cmake_cache(out_path)

    if target:
        ninja_target = [target]
    else:
        ninja_target = []

    print subprocess.list2cmdline(flags)
    subprocess.check_call(
        [cmake_bin_path()] + flags, cwd=out_path, env=env)
    subprocess.check_call(
        [ninja_bin_path()] + ninja_target, cwd=out_path, env=env)
    subprocess.check_call(
        [ninja_bin_path(), 'install'], cwd=out_path, env=env)


def build_crts(base_cmake_defines, stage2_install):
    cc = os.path.join(stage2_install, 'bin', 'clang')
    cxx = os.path.join(stage2_install, 'bin', 'clang++')
    llvm_config = os.path.join(stage2_install, 'bin', 'llvm-config')

    crt_configs = [
        # Bug: http://b/35404115: Use armv7 triple - some assembly sources in
        # builtins fail to build with arm
        ('arm', 'arm',
            'arm/arm-linux-androideabi-4.9/arm-linux-androideabi',
            'armv7-linux-android', ''),
        ('aarch64', 'arm64',
            'aarch64/aarch64-linux-android-4.9/aarch64-linux-android',
            'aarch64-linux-android', ''),
        ('x86_64', 'x86_64',
            'x86/x86_64-linux-android-4.9/x86_64-linux-android',
            'x86_64-linux-android', ''),
        ('x86', 'x86',
            'x86/x86_64-linux-android-4.9/x86_64-linux-android',
            'i686-linux-android', '-m32'),
        # Disable mips32 and mips64 because they don't build.
        # ('mips', 'mips',
        #     'mips/mips64el-linux-android-4.9/mips64el-linux-android',
        #     'mipsel-linux-android', '-m32'),
        # ('mips64', 'mips64',
        #     'mips/mips64el-linux-android-4.9/mips64el-linux-android',
        #     'mips64el-linux-android', '-m64'),
        ]

    # Now build compiler-rt for each arch
    for (arch, ndk_arch, toolchain_path, llvm_triple, extra_flags) in crt_configs:
        print "Building compiler-rt for %s" % arch
        crt_path = android_path('out', 'clangrt-' + arch)
        crt_install = os.path.join(stage2_install, 'lib', 'clang', clang_version())

        toolchain_root = android_path('prebuilts/gcc', build_os_type())
        toolchain_bin = os.path.join(toolchain_root, toolchain_path, 'bin')
        sysroot = os.path.join(ndk_path(), 'arch-' + ndk_arch)

        # Bug: http://b/35402623: Manually include the directory with libgcc.a.
        # For some reason, it is not found automatically
        toolchain_builtins = os.path.join(toolchain_root, toolchain_path, '..',
                                          'lib', 'gcc',
                                          os.path.basename(toolchain_path),
                                          '4.9.x')
        # The 32-bit libgcc.a is in a separate subdir
        if arch == 'x86':
            toolchain_builtins = os.path.join(toolchain_builtins, '32')


        cflags = ['--target=%s' % llvm_triple,
                  '--sysroot=%s' % sysroot,
                  '-B%s' % toolchain_bin,
                  '-L%s' % toolchain_builtins,
                  # Bug: http://b/35402623 Clang warns that the -L... above is
                  # unused in compile-only invocations.
                  '-Wno-unused-command-line-argument',
                  extra_flags,
                 ]
        cxxflags = cflags[:]

        crt_defines = base_cmake_defines.copy()
        crt_defines['ANDROID'] = '1'
        crt_defines['LLVM_CONFIG_PATH'] = llvm_config
        crt_defines['COMPILER_RT_INCLUDE_TESTS'] = 'ON'
        crt_defines['COMPILER_RT_ENABLE_WERROR'] = 'ON'
        crt_defines['CMAKE_C_COMPILER'] = cc
        crt_defines['CMAKE_CXX_COMPILER'] = cxx
        crt_defines['CMAKE_C_FLAGS'] = ' '.join(cflags)
        crt_defines['CMAKE_ASM_FLAGS'] = ' '.join(cflags)
        crt_defines['CMAKE_CXX_FLAGS'] = ' '.join(cxxflags)
        crt_defines['COMPILER_RT_TEST_COMPILER_CFLAGS'] = ' '.join(cflags)
        crt_defines['COMPILER_RT_TEST_TARGET_TRIPLE'] = llvm_triple
        crt_defines['COMPILER_RT_INCLUDE_TESTS'] = 'OFF'
        crt_defines['CMAKE_INSTALL_PREFIX'] = crt_install

        crt_env = dict(ORIG_ENV)

        crt_cmake_path = os.path.join(android_path('llvm'), 'projects',
                                      'compiler-rt')
        rm_cmake_cache(crt_path)
        build_llvm(out_path=crt_path, defines=crt_defines, env=crt_env,
                cmake_path=crt_cmake_path)


def main():
    cflags = []
    cxxflags = []
    ldflags = []

    base_cmake_defines = {}

    base_cmake_defines['CMAKE_BUILD_TYPE'] = 'Release'
    base_cmake_defines['LLVM_ENABLE_ASSERTIONS'] = 'ON'
    base_cmake_defines['LLVM_ENABLE_THREADS'] = 'OFF'

    # Construct the stage 1 defines
    stage1_path = android_path('out', 'stage1')
    stage1_install = android_path('out', 'stage1-install')

    stage1_defines = base_cmake_defines.copy()
    stage1_defines['CMAKE_INSTALL_PREFIX'] = stage1_install
    stage1_defines['LLVM_TARGETS_TO_BUILD'] = 'X86'
    stage1_defines['CMAKE_C_FLAGS'] = ' '.join(cflags)
    stage1_defines['CMAKE_CXX_FLAGS'] = ' '.join(cxxflags)
    stage1_defines['CMAKE_EXE_LINKER_FLAGS'] = ' '.join(ldflags)
    stage1_defines['CMAKE_SHARED_LINKER_FLAGS'] = ' '.join(ldflags)
    stage1_defines['CMAKE_MODULE_LINKER_FLAGS'] = ' '.join(ldflags)
    stage1_defines['CLANG_VENDOR'] = 'Android '

    stage1_env = dict(ORIG_ENV)

    # Build/install the stage 1 toolchain
    build_llvm(out_path=stage1_path, defines=stage1_defines, env=stage1_env,
            cmake_path=android_path('llvm'))

    # TODO(srhines): Build LTO plugin (Chromium folks say ~10% perf speedup)

    # Construct the stage 2 defines
    cc = os.path.join(stage1_install, 'bin', 'clang')
    cxx = os.path.join(stage1_install, 'bin', 'clang++')
    stage2_path = android_path('out', 'stage2')
    stage2_install = android_path('out', 'stage2-install')

    stage2_defines = base_cmake_defines.copy()
    stage2_defines['CMAKE_INSTALL_PREFIX'] = stage2_install
    stage2_defines['LLVM_TARGETS_TO_BUILD'] = 'AArch64;ARM;Mips;X86'
    stage2_defines['CMAKE_C_COMPILER'] = cc
    stage2_defines['CMAKE_CXX_COMPILER'] = cxx
    stage2_defines['CMAKE_C_FLAGS'] = ' '.join(cflags)
    stage2_defines['CMAKE_CXX_FLAGS'] = ' '.join(cxxflags)
    stage2_defines['CMAKE_EXE_LINKER_FLAGS'] = ' '.join(ldflags)
    stage2_defines['CMAKE_SHARED_LINKER_FLAGS'] = ' '.join(ldflags)
    stage2_defines['CMAKE_MODULE_LINKER_FLAGS'] = ' '.join(ldflags)
    stage2_defines['CLANG_VENDOR'] = 'Android '
    # Compiler-rt builtins and sanitizer runtimes are built separately
    stage2_defines['LLVM_BUILD_EXTERNAL_COMPILER_RT'] = 'ON'

    stage2_env = dict(ORIG_ENV)

    build_llvm(out_path=stage2_path, defines=stage2_defines, env=stage2_env,
            cmake_path=android_path('llvm'))

    if build_os_type() == 'linux-x86':
        build_crts(base_cmake_defines, stage2_install)

    return 0


if __name__ == '__main__':
    main()
