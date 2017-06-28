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
import utils

from version import Version

ORIG_ENV = dict(os.environ)
STAGE2_TARGETS = 'AArch64;ARM;Mips;X86'


def extract_clang_version(stage2_install):
    version_file = os.path.join(stage2_install, 'include', 'clang', 'Basic',
                                'Version.inc')
    return Version(version_file)


def ndk_path():
    # TODO Switch to r13 from the toolchain/prebuilts/ndk/r13 branch
    ndk_version = 'r10'
    platform_level = 'android-23'
    return utils.android_path('prebuilts/ndk', ndk_version, 'platforms',
                              platform_level)


# This is the baseline stable version of Clang to start our stage-1 build.
def clang_prebuilt_version():
    return 'clang-4053586'


def clang_prebuilt_base_dir():
    return utils.android_path('prebuilts/clang/host', utils.build_os_type(),
                              clang_prebuilt_version())

def clang_prebuilt_bin_dir():
    return utils.android_path(clang_prebuilt_base_dir(), 'bin')


def clang_prebuilt_lib_dir():
    return utils.android_path(clang_prebuilt_base_dir(), 'lib64')


def cmake_prebuilt_bin_dir():
    return utils.android_path('prebuilts/cmake', utils.build_os_type(), 'bin')


def cmake_bin_path():
    return os.path.join(cmake_prebuilt_bin_dir(), 'cmake')


def ninja_bin_path():
    return os.path.join(cmake_prebuilt_bin_dir(), 'ninja')


def check_create_path(path):
    if not os.path.exists(path):
        os.makedirs(path)


def rm_cmake_cache(dir):
    for dirpath, dirs, files in os.walk(dir):
        if 'CMakeCache.txt' in files:
            os.remove(os.path.join(dirpath, 'CMakeCache.txt'))
        if 'CMakeFiles' in dirs:
            utils.rm_tree(os.path.join(dirpath, 'CMakeFiles'))


# Base cmake options such as build type that are common across all invocations
def base_cmake_defines():
    defines = {}

    defines['CMAKE_BUILD_TYPE'] = 'Release'
    defines['LLVM_ENABLE_ASSERTIONS'] = 'ON'
    defines['LLVM_ENABLE_THREADS'] = 'OFF'
    defines['LLVM_LIBDIR_SUFFIX'] = '64'
    return defines


def invoke_cmake(out_path, defines, env, cmake_path, target=None, install=True):
    flags = ['-G', 'Ninja']

    # Specify CMAKE_PREFIX_PATH so 'cmake -G Ninja ...' can find the ninja
    # executable.
    flags += ['-DCMAKE_PREFIX_PATH=' + cmake_prebuilt_bin_dir()]

    for key in defines:
        newdef = '-D' + key + '=' + defines[key]
        flags += [newdef]
    flags += [cmake_path]

    check_create_path(out_path)
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
    if install:
        subprocess.check_call(
            [ninja_bin_path(), 'install'], cwd=out_path, env=env)


def cross_compile_configs(stage2_install):
    configs = [
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
        ('i386', 'x86',
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

    cc = os.path.join(stage2_install, 'bin', 'clang')
    cxx = os.path.join(stage2_install, 'bin', 'clang++')

    for (arch, ndk_arch, toolchain_path, llvm_triple, extra_flags) in configs:
        toolchain_root = utils.android_path('prebuilts/gcc',
                                            utils.build_os_type())
        toolchain_bin = os.path.join(toolchain_root, toolchain_path, 'bin')
        sysroot = os.path.join(ndk_path(), 'arch-' + ndk_arch)

        defines = {}
        defines['CMAKE_C_COMPILER'] = cc
        defines['CMAKE_CXX_COMPILER'] = cxx

        # Include the directory with libgcc.a to the linker search path.
        toolchain_builtins = os.path.join(toolchain_root, toolchain_path, '..',
                                          'lib', 'gcc',
                                          os.path.basename(toolchain_path),
                                          '4.9.x')
        # The 32-bit libgcc.a is in a separate subdir
        if arch == 'i386':
            toolchain_builtins = os.path.join(toolchain_builtins, '32')
        ldflags = ['-L' + toolchain_builtins]
        defines['CMAKE_EXE_LINKER_FLAGS'] = ' '.join(ldflags)
        defines['CMAKE_SHARED_LINKER_FLAGS'] = ' '.join(ldflags)
        defines['CMAKE_MODULE_LINKER_FLAGS'] = ' '.join(ldflags)

        cflags = ['--target=%s' % llvm_triple,
                  '--sysroot=%s' % sysroot,
                  '-B%s' % toolchain_bin,
                  extra_flags,
                 ]
        yield (arch, llvm_triple, defines, cflags)


def build_asan_test(stage2_install):
    # We can not build asan_test using current CMake building system. Since
    # those files are not used to build AOSP, we just simply touch them so that
    # we can pass the build checks.
    for arch in ('aarch64', 'arm', 'i686', 'mips', 'mips64'):
        asan_test_path = os.path.join(stage2_install, 'test', arch, 'bin')
        check_create_path(asan_test_path)
        asan_test_bin_path = os.path.join(asan_test_path, 'asan_test')
        open(asan_test_bin_path, 'w+').close()


def build_libcxx(stage2_install, clang_version):
    support_headers = utils.android_path('bionic', 'libc', 'include')
    for (arch, llvm_triple, libcxx_defines, cflags) in cross_compile_configs(stage2_install):
        print "Building libcxx for %s" % arch
        libcxx_path = utils.android_path('out', 'lib', 'libcxx-'+arch)

        cflags.extend(['-isystem %s' % support_headers])

        libcxx_defines['CMAKE_C_FLAGS'] = ' '.join(cflags)
        libcxx_defines['CMAKE_CXX_FLAGS'] = ' '.join(cflags)
        libcxx_defines['CMAKE_BUILD_TYPE'] = 'Release'

        libcxx_env = dict(ORIG_ENV)

        libcxx_cmake_path = utils.llvm_path('projects', 'libcxx')
        rm_cmake_cache(libcxx_path)

        invoke_cmake(out_path=libcxx_path, defines=libcxx_defines,
                     env=libcxx_env, cmake_path=libcxx_cmake_path,
                     install=False)
        # We need to install libcxx manually.
        libcxx_install = os.path.join(stage2_install, 'lib64', 'clang',
                                      clang_version.short_version(), 'lib',
                                      'linux', arch)
        libcxx_libs = os.path.join(libcxx_path, 'lib')
        check_create_path(libcxx_install)
        for f in os.listdir(libcxx_libs):
            if f.startswith('libc++'):
                shutil.copy2(os.path.join(libcxx_libs, f), libcxx_install)



def build_crts(stage2_install, clang_version):
    llvm_config = os.path.join(stage2_install, 'bin', 'llvm-config')
    # Now build compiler-rt for each arch
    for (arch, llvm_triple, crt_defines, cflags) in cross_compile_configs(stage2_install):
        print "Building compiler-rt for %s" % arch
        crt_path = utils.android_path('out', 'lib', 'clangrt-'+arch)
        crt_install = os.path.join(stage2_install, 'lib64', 'clang',
                                   clang_version.short_version())

        crt_defines['ANDROID'] = '1'
        crt_defines['LLVM_CONFIG_PATH'] = llvm_config
        crt_defines['COMPILER_RT_INCLUDE_TESTS'] = 'ON'
        crt_defines['COMPILER_RT_ENABLE_WERROR'] = 'ON'
        crt_defines['CMAKE_C_FLAGS'] = ' '.join(cflags)
        crt_defines['CMAKE_ASM_FLAGS'] = ' '.join(cflags)
        crt_defines['CMAKE_CXX_FLAGS'] = ' '.join(cflags)
        crt_defines['COMPILER_RT_TEST_COMPILER_CFLAGS'] = ' '.join(cflags)
        crt_defines['COMPILER_RT_TEST_TARGET_TRIPLE'] = llvm_triple
        crt_defines['COMPILER_RT_INCLUDE_TESTS'] = 'OFF'
        crt_defines['CMAKE_INSTALL_PREFIX'] = crt_install
        crt_defines.update(base_cmake_defines())

        crt_env = dict(ORIG_ENV)

        crt_cmake_path = utils.llvm_path('projects', 'compiler-rt')
        rm_cmake_cache(crt_path)
        invoke_cmake(out_path=crt_path, defines=crt_defines, env=crt_env,
                cmake_path=crt_cmake_path)


def build_libfuzzers(stage2_install, clang_version):
    libcxx_headers = utils.llvm_path('projects', 'libcxx', 'include')
    support_headers = utils.android_path('bionic', 'libc', 'include')

    for (arch, llvm_triple, libfuzzer_defines, cflags) in cross_compile_configs(stage2_install):
        print "Building libfuzzer for %s" % arch
        libfuzzer_path = utils.android_path('out', 'lib', 'libfuzzer-'+arch)
        libfuzzer_defines['CMAKE_BUILD_TYPE'] = 'Release'
        libfuzzer_defines['LLVM_USE_SANITIZER'] = 'Address'
        libfuzzer_defines['LLVM_USE_SANITIZE_COVERAGE'] = 'YES'
        libfuzzer_defines['CMAKE_CXX_STANDARD'] = '11'

        cflags.extend(['-isystem %s' % libcxx_headers,
                       '-isystem %s' % support_headers])

        libfuzzer_defines['CMAKE_C_FLAGS'] = ' '.join(cflags)
        libfuzzer_defines['CMAKE_CXX_FLAGS'] = ' '.join(cflags)

        # lib/Fuzzer/CMakeLists.txt does not call cmake_minimum_required() to
        # set a minimum version.  Explicitly request a policy that'll pass
        # CMAKE_*_LINKER_FLAGS to the trycompile() step.
        libfuzzer_defines['CMAKE_POLICY_DEFAULT_CMP0056'] = 'NEW'

        libfuzzer_cmake_path = utils.llvm_path('lib', 'Fuzzer')
        libfuzzer_env = dict(ORIG_ENV)
        rm_cmake_cache(libfuzzer_path)
        invoke_cmake(out_path=libfuzzer_path, defines=libfuzzer_defines,
                     env=libfuzzer_env, cmake_path=libfuzzer_cmake_path,
                     install=False)
        # We need to install libfuzzer manually.
        static_lib = os.path.join(libfuzzer_path, 'libLLVMFuzzer.a')
        lib_dir = os.path.join(stage2_install, 'lib64', 'clang',
                               clang_version.short_version(), 'lib',
                               'linux', arch)
        check_create_path(lib_dir)
        shutil.copy2(static_lib, os.path.join(lib_dir, 'libFuzzer.a'))

    # Install libfuzzer headers.
    header_src = utils.llvm_path('lib', 'Fuzzer')
    header_dst = os.path.join(stage2_install, 'prebuilt_include', 'llvm',
                              'lib', 'Fuzzer')
    check_create_path(header_dst)
    for f in os.listdir(header_src):
        if f.endswith('.h') or f.endswith('.def'):
            shutil.copy2(os.path.join(header_src, f), header_dst)


def build_llvm(targets, build_dir, install_dir, extra_defines=None,
               extra_env=None):
    cmake_defines = base_cmake_defines()
    cmake_defines['CMAKE_INSTALL_PREFIX'] = install_dir
    cmake_defines['LLVM_TARGETS_TO_BUILD'] = targets
    cmake_defines['CLANG_VENDOR'] = 'Android '

    if extra_defines is not None:
        cmake_defines.update(extra_defines)

    env = dict(ORIG_ENV)
    if extra_env is not None:
        env.update(extra_env)

    invoke_cmake(out_path=build_dir, defines=cmake_defines, env=env,
                 cmake_path=utils.llvm_path())


def build_llvm_for_windows(targets, build_dir, install_dir,
                           native_clang_install, is_32_bit=False):

    mingw_path = utils.android_path('prebuilts', 'gcc', 'linux-x86', 'host',
                                    'x86_64-w64-mingw32-4.8')
    mingw_cc = os.path.join(mingw_path, 'bin', 'x86_64-w64-mingw32-gcc')
    mingw_cxx = os.path.join(mingw_path, 'bin', 'x86_64-w64-mingw32-g++')

    # Write a NATIVE.cmake in windows_path that contains the compilers used
    # to build native tools such as llvm-tblgen and llvm-config.  This is
    # used below via the CMake variable CROSS_TOOLCHAIN_FLAGS_NATIVE.
    native_clang_cc = os.path.join(native_clang_install, 'bin', 'clang')
    native_clang_cxx = os.path.join(native_clang_install, 'bin', 'clang++')
    check_create_path(build_dir)
    native_cmake_file_path = os.path.join(build_dir, 'NATIVE.cmake')
    native_cmake_text = (
        'set(CMAKE_C_COMPILER {cc})\n'
        'set(CMAKE_CXX_COMPILER {cxx})\n'
    ).format(cc=native_clang_cc, cxx=native_clang_cxx)

    with open(native_cmake_file_path, 'w') as native_cmake_file:
        native_cmake_file.write(native_cmake_text)

    # Extra cmake defines to use while building for Windows
    windows_extra_defines = dict()
    windows_extra_defines['CMAKE_C_COMPILER'] = mingw_cc
    windows_extra_defines['CMAKE_CXX_COMPILER'] = mingw_cxx
    windows_extra_defines['CMAKE_SYSTEM_NAME'] = 'Windows'
    # Don't buld compiler-rt, libcxx etc. for Windows
    windows_extra_defines['LLVM_BUILD_RUNTIME'] = 'OFF'
    windows_extra_defines['LLVM_TOOL_CLANG_TOOLS_EXTRA_BUILD'] = 'OFF'
    windows_extra_defines['LLVM_TOOL_OPENMP_BUILD'] = 'OFF'

    windows_extra_defines['CROSS_TOOLCHAIN_FLAGS_NATIVE'] = \
        '-DCMAKE_PREFIX_PATH=' + cmake_prebuilt_bin_dir() + ';' + \
        '-DCMAKE_TOOLCHAIN_FILE=' + native_cmake_file_path

    if is_32_bit:
        cflags = ['-m32']
        cxxflags = ['-m32']
        ldflags = ['-m32']

        windows_extra_defines['CMAKE_C_FLAGS'] = ' '.join(cflags)
        windows_extra_defines['CMAKE_CXX_FLAGS'] = ' '.join(cxxflags)
        windows_extra_defines['CMAKE_EXE_LINKER_FLAGS'] = ' '.join(ldflags)
        windows_extra_defines['CMAKE_SHARED_LINKER_FLAGS'] = ' '.join(ldflags)
        windows_extra_defines['CMAKE_MODULE_LINKER_FLAGS'] = ' '.join(ldflags)

    build_llvm(targets=targets, build_dir=build_dir, install_dir=install_dir,
               extra_defines=windows_extra_defines)


def build_stage1():
    # Build/install the stage 1 toolchain
    stage1_path = utils.android_path('out', 'stage1')
    stage1_install = utils.android_path('out', 'stage1-install')
    stage1_targets = 'X86'

    stage1_extra_defines = dict()
    stage1_extra_defines['LLVM_BUILD_TOOLS'] = 'OFF'
    stage1_extra_defines['LLVM_BUILD_RUNTIME'] = 'ON'
    stage1_extra_defines['CLANG_ENABLE_ARCMT'] = 'OFF'
    stage1_extra_defines['CLANG_ENABLE_STATIC_ANALYZER'] = 'OFF'
    stage1_extra_defines['CMAKE_C_COMPILER'] = os.path.join(
            clang_prebuilt_bin_dir(), 'clang')
    stage1_extra_defines['CMAKE_CXX_COMPILER'] = os.path.join(
            clang_prebuilt_bin_dir(), 'clang++')
    stage1_extra_defines['LLVM_TOOL_CLANG_TOOLS_EXTRA_BUILD'] = 'OFF'
    stage1_extra_defines['LLVM_TOOL_OPENMP_BUILD'] = 'OFF'

    # Have clang use libc++, ...
    stage1_extra_defines['LLVM_ENABLE_LIBCXX'] = 'ON'

    # ... and point CMake to the libc++.so from the prebuilts.  Install an rpath
    # to prevent linking with the newly-built libc++.so
    ldflags = ['-Wl,-rpath,' + clang_prebuilt_lib_dir()]
    stage1_extra_defines['CMAKE_EXE_LINKER_FLAGS'] = ' '.join(ldflags)
    stage1_extra_defines['CMAKE_SHARED_LINKER_FLAGS'] = ' '.join(ldflags)
    stage1_extra_defines['CMAKE_MODULE_LINKER_FLAGS'] = ' '.join(ldflags)

    # Make libc++.so a symlink to libc++.so.x instead of a linker script that
    # also adds -lc++abi.  Statically link libc++abi to libc++ so it is not
    # necessary to pass -lc++abi explicitly.  This is needed only for Linux.
    if utils.build_os_type() == 'linux-x86':
        stage1_extra_defines['LIBCXX_ENABLE_ABI_LINKER_SCRIPT'] = 'OFF'
        stage1_extra_defines['LIBCXX_ENABLE_STATIC_ABI_LIBRARY'] = 'ON'

    # Do not build compiler-rt for Darwin.  We don't ship host (or any
    # prebuilt) runtimes for Darwin anyway.  Attempting to build these will
    # fail compilation of lib/builtins/atomic_*.c that only get built for
    # Darwin and fail compilation due to us using the bionic version of
    # stdatomic.h.
    if utils.build_os_type() == 'darwin-x86':
          stage1_extra_defines['LLVM_BUILD_EXTERNAL_COMPILER_RT'] = 'ON'

    build_llvm(targets=stage1_targets, build_dir=stage1_path,
               install_dir=stage1_install, extra_defines=stage1_extra_defines)
    return stage1_install


def build_stage2(stage1_install, stage2_targets):
    # TODO(srhines): Build LTO plugin (Chromium folks say ~10% perf speedup)

    # Build/install the stage2 toolchain
    stage2_cc = os.path.join(stage1_install, 'bin', 'clang')
    stage2_cxx = os.path.join(stage1_install, 'bin', 'clang++')
    stage2_path = utils.android_path('out', 'stage2')
    stage2_install = utils.android_path('out', 'stage2-install')

    stage2_extra_defines = dict()
    stage2_extra_defines['CMAKE_C_COMPILER'] = stage2_cc
    stage2_extra_defines['CMAKE_CXX_COMPILER'] = stage2_cxx
    stage2_extra_defines['LLVM_BUILD_RUNTIME'] = 'ON'
    stage2_extra_defines['LLVM_ENABLE_LIBCXX'] = 'ON'
    stage2_extra_defines['LIBCXX_ENABLE_STATIC_ABI_LIBRARY'] = 'ON'
    stage2_extra_defines['LIBCXX_ENABLE_ABI_LINKER_SCRIPT'] = 'OFF'

    # Make libc++.so a symlink to libc++.so.x instead of a linker script that
    # also adds -lc++abi.  Statically link libc++abi to libc++ so it is not
    # necessary to pass -lc++abi explicitly.  This is needed only for Linux.
    if utils.build_os_type() == 'linux-x86':
        stage2_extra_defines['LIBCXX_ENABLE_STATIC_ABI_LIBRARY'] = 'ON'
        stage2_extra_defines['LIBCXX_ENABLE_ABI_LINKER_SCRIPT'] = 'OFF'

    # Do not build compiler-rt for Darwin.  We don't ship host (or any
    # prebuilt) runtimes for Darwin anyway.  Attempting to build these will
    # fail compilation of lib/builtins/atomic_*.c that only get built for
    # Darwin and fail compilation due to us using the bionic version of
    # stdatomic.h.
    if utils.build_os_type == 'darwin-x86':
        stage2_extra_defines['LLVM_BUILD_EXTERNAL_COMPILER_RT'] = 'ON'

    # Point CMake to the libc++ from stage1.  It is possible that once built,
    # the newly-built libc++ may override this because of the rpath pointing to
    # $ORIGIN/../lib64.  That'd be fine because both libraries are built from
    # the same sources.
    stage2_extra_env = dict()
    stage2_extra_env['LD_LIBRARY_PATH'] = os.path.join(stage1_install, 'lib64')

    build_llvm(targets=stage2_targets, build_dir=stage2_path,
               install_dir=stage2_install, extra_defines=stage2_extra_defines,
               extra_env=stage2_extra_env)
    return stage2_install


def build_runtimes(stage2_install):
    version = extract_clang_version(stage2_install)
    build_crts(stage2_install, version)
    build_libfuzzers(stage2_install, version)
    build_libcxx(stage2_install, version)
    build_asan_test(stage2_install)


def main():
    stage1_install = build_stage1()
    stage2_install = build_stage2(stage1_install, STAGE2_TARGETS)

    if utils.build_os_type() == 'linux-x86':
        build_runtimes(stage2_install)

        # Build single-stage clang for Windows
        windows_targets = STAGE2_TARGETS

        # Build 64-bit clang for Windows
        windows64_path  = utils.android_path('out', 'windows-x86')
        windows64_install = utils.android_path('out', 'windows-x86-install')
        build_llvm_for_windows(targets=windows_targets,
                               build_dir=windows64_path,
                               install_dir=windows64_install,
                               native_clang_install=stage2_install)

        # Build 32-bit clang for Windows
        windows32_path = utils.android_path('out', 'windows-i386')
        windows32_install = utils.android_path('out', 'windows-i386')
        build_llvm_for_windows(targets=windows_targets,
                               build_dir=windows32_path,
                               install_dir=windows32_install,
                               native_clang_install=stage2_install,
                               is_32_bit=True)

    return 0


if __name__ == '__main__':
    main()
