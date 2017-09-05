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

import argparse
import logging
import os
import shutil
import subprocess
import utils

from version import Version

ORIG_ENV = dict(os.environ)
STAGE2_TARGETS = 'AArch64;ARM;Mips;X86'


def logger():
    """Returns the module level logger."""
    return logging.getLogger(__name__)


def check_call(cmd, *args, **kwargs):
    """subprocess.check_call with logging."""
    logger().info('check_call: %s', subprocess.list2cmdline(cmd))
    subprocess.check_call(cmd, *args, **kwargs)


def install_file(src, dst):
    """Proxy for shutil.copy2 with logging and dry-run support."""
    import shutil
    logger().info('copy %s %s', src, dst)
    shutil.copy2(src, dst)


def remove(path):
    """Proxy for os.remove with logging."""
    logger().debug('remove %s', path)
    os.remove(path)


def extract_clang_version(stage2_install):
    version_file = os.path.join(stage2_install, 'include', 'clang', 'Basic',
                                'Version.inc')
    return Version(version_file)


def ndk_base():
    ndk_version = 'r15'
    return utils.android_path('toolchain/prebuilts/ndk', ndk_version)


def android_api(arch):
    if arch in ['arm', 'i386', 'mips']:
        return '14'
    else:
        return '21'


def ndk_path(arch):
    platform_level = 'android-' + android_api(arch)
    return os.path.join(ndk_base(), 'platforms', platform_level)


def libcxx_headers():
    return os.path.join(ndk_base(), 'sources', 'cxx-stl', 'llvm-libc++',
                        'include')


def libcxxabi_headers():
    return os.path.join(ndk_base(), 'sources', 'cxx-stl', 'llvm-libc++abi',
                        'include')


def ndk_toolchain_lib(arch, toolchain_root, host_tag):
    toolchain_lib = os.path.join(ndk_base(), 'toolchains', toolchain_root,
                                 'prebuilt', 'linux-x86_64', host_tag)
    if arch in ['arm', 'i386', 'mips']:
        toolchain_lib = os.path.join(toolchain_lib, 'lib')
    else:
        toolchain_lib = os.path.join(toolchain_lib, 'lib64')
    return toolchain_lib


def support_headers():
    return os.path.join(ndk_base(), 'sources', 'android', 'support', 'include')


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

    check_call([cmake_bin_path()] + flags, cwd=out_path, env=env)
    check_call([ninja_bin_path()] + ninja_target, cwd=out_path, env=env)
    if install:
        check_call([ninja_bin_path(), 'install'], cwd=out_path, env=env)


def cross_compile_configs(stage2_install):
    configs = [
        # Bug: http://b/35404115: Switch to armv7-linux-android once armv5 is
        # deprecated from the NDK
        ('arm', 'arm',
            'arm/arm-linux-androideabi-4.9/arm-linux-androideabi',
            'arm-linux-android', ''),
        ('aarch64', 'arm64',
            'aarch64/aarch64-linux-android-4.9/aarch64-linux-android',
            'aarch64-linux-android', ''),
        ('x86_64', 'x86_64',
            'x86/x86_64-linux-android-4.9/x86_64-linux-android',
            'x86_64-linux-android', ''),
        ('i386', 'x86',
            'x86/x86_64-linux-android-4.9/x86_64-linux-android',
            'i686-linux-android', '-m32'),
        ('mips', 'mips',
            'mips/mips64el-linux-android-4.9/mips64el-linux-android',
            'mipsel-linux-android', '-m32'),
        ('mips64', 'mips64',
            'mips/mips64el-linux-android-4.9/mips64el-linux-android',
            'mips64el-linux-android', '-m64'),
        ]

    cc = os.path.join(stage2_install, 'bin', 'clang')
    cxx = os.path.join(stage2_install, 'bin', 'clang++')

    for (arch, ndk_arch, toolchain_path, llvm_triple, extra_flags) in configs:
        toolchain_root = utils.android_path('prebuilts/gcc',
                                            utils.build_os_type())
        toolchain_bin = os.path.join(toolchain_root, toolchain_path, 'bin')
        sysroot_libs = os.path.join(ndk_path(arch), 'arch-' + ndk_arch)
        sysroot = os.path.join(ndk_base(), 'sysroot')
        if arch == 'arm':
            sysroot_headers = os.path.join(sysroot, 'usr', 'include',
                                           'arm-linux-androideabi')
        else:
            sysroot_headers = os.path.join(sysroot, 'usr', 'include',
                                           llvm_triple)

        defines = {}
        defines['CMAKE_C_COMPILER'] = cc
        defines['CMAKE_CXX_COMPILER'] = cxx

        # Bug: http://b/35404115: Re-enable builtins for arm32 once armv5 is
        # deprecated from the NDK
        if arch == 'arm':
            defines['COMPILER_RT_BUILD_BUILTINS'] = 'OFF'

        # Include the directory with libgcc.a to the linker search path.
        toolchain_builtins = os.path.join(toolchain_root, toolchain_path, '..',
                                          'lib', 'gcc',
                                          os.path.basename(toolchain_path),
                                          '4.9.x')
        # The 32-bit libgcc.a is sometimes in a separate subdir
        if arch == 'i386':
            toolchain_builtins = os.path.join(toolchain_builtins, '32')
        elif arch == 'mips':
            toolchain_builtins = os.path.join(toolchain_builtins, '32',
                                              'mips-r2')
        libcxx_libs = os.path.join(ndk_base(), 'sources', 'cxx-stl',
                                   'llvm-libc++', 'libs')
        if ndk_arch == 'arm':
            libcxx_libs = os.path.join(libcxx_libs, 'armeabi')
        elif ndk_arch == 'arm64':
            libcxx_libs = os.path.join(libcxx_libs, 'arm64-v8a')
        else:
            libcxx_libs = os.path.join(libcxx_libs, ndk_arch)

        if ndk_arch == 'arm':
            toolchain_lib = ndk_toolchain_lib(arch, 'arm-linux-androideabi-4.9',
                                              'arm-linux-androideabi')
        elif ndk_arch == 'x86' or ndk_arch == 'x86_64':
            toolchain_lib = ndk_toolchain_lib(arch, ndk_arch + '-4.9',
                                              llvm_triple)
        else:
            toolchain_lib = ndk_toolchain_lib(arch, llvm_triple + '-4.9',
                                              llvm_triple)

        ldflags = ['-L' + toolchain_builtins, '-Wl,-z,defs', '-L' + libcxx_libs,
                   '-L' + toolchain_lib, '--sysroot=%s' % sysroot_libs]
        defines['CMAKE_EXE_LINKER_FLAGS'] = ' '.join(ldflags)
        defines['CMAKE_SHARED_LINKER_FLAGS'] = ' '.join(ldflags)
        defines['CMAKE_MODULE_LINKER_FLAGS'] = ' '.join(ldflags)
        defines['CMAKE_SYSROOT'] = sysroot
        defines['CMAKE_SYSROOT_COMPILE'] = sysroot

        cflags = ['--target=%s' % llvm_triple,
                  '-B%s' % toolchain_bin,
                  '-isystem %s' % sysroot_headers,
                  '-D__ANDROID_API__=%s' % android_api(arch),
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
    for (arch, llvm_triple, libcxx_defines, cflags) in cross_compile_configs(stage2_install):
        logger().info('Building libcxx for %s', arch)
        libcxx_path = utils.android_path('out', 'lib', 'libcxx-' + arch)

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
        logger().info('Building compiler-rt for %s', arch)
        crt_path = utils.android_path('out', 'lib', 'clangrt-' + arch)
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

        # libcxxabi is statically linked into libc++ and we need to add libc++
        # manually here.
        crt_defines['SANITIZER_CXX_ABI'] = 'libc++'
        # The following two lines can be removed after r309074.
        crt_defines['ASAN_DYNAMIC_LIBS'] = 'c++'
        crt_defines['UBSAN_DYNAMIC_LIBS'] = 'c++'
        crt_defines.update(base_cmake_defines())

        crt_env = dict(ORIG_ENV)

        crt_cmake_path = utils.llvm_path('projects', 'compiler-rt')
        rm_cmake_cache(crt_path)
        invoke_cmake(out_path=crt_path, defines=crt_defines, env=crt_env,
                cmake_path=crt_cmake_path)


def build_libfuzzers(stage2_install, clang_version):

    for (arch, llvm_triple, libfuzzer_defines, cflags) in cross_compile_configs(stage2_install):
        logger().info('Building libfuzzer for %s', arch)
        libfuzzer_path = utils.android_path('out', 'lib', 'libfuzzer-' + arch)
        libfuzzer_defines['CMAKE_BUILD_TYPE'] = 'Release'
        libfuzzer_defines['LLVM_USE_SANITIZER'] = 'Address'
        libfuzzer_defines['LLVM_USE_SANITIZE_COVERAGE'] = 'YES'
        libfuzzer_defines['CMAKE_CXX_STANDARD'] = '11'

        cflags.extend(['-isystem %s' % libcxx_headers(),
                       '-isystem %s' % libcxxabi_headers(),
                       '-isystem %s' % support_headers()])

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
    cmake_defines['LLVM_BUILD_LLVM_DYLIB'] = 'ON'
    cmake_defines['CLANG_VENDOR'] = 'Android '
    cmake_defines['LLVM_BINUTILS_INCDIR'] = utils.android_path(
        'toolchain/binutils/binutils-2.25/include')

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
    # Don't build compiler-rt, libcxx etc. for Windows
    windows_extra_defines['LLVM_BUILD_RUNTIME'] = 'OFF'
    # Build clang-tidy/clang-format for Windows.
    windows_extra_defines['LLVM_TOOL_CLANG_TOOLS_EXTRA_BUILD'] = 'ON'
    windows_extra_defines['LLVM_TOOL_OPENMP_BUILD'] = 'OFF'

    windows_extra_defines['CROSS_TOOLCHAIN_FLAGS_NATIVE'] = \
        '-DCMAKE_PREFIX_PATH=' + cmake_prebuilt_bin_dir() + ';' + \
        '-DCMAKE_TOOLCHAIN_FILE=' + native_cmake_file_path

    # http://b/62787860 - Change the ar flags, because mingw has issues with
    # duplicates created when using the default "qc" flags.
    windows_extra_defines['CMAKE_CXX_ARCHIVE_CREATE'] = \
        '<CMAKE_AR> crsPD <TARGET> <LINK_FLAGS> <OBJECTS>'
    windows_extra_defines['CMAKE_C_ARCHIVE_CREATE'] = \
        '<CMAKE_AR> crsPD <TARGET> <LINK_FLAGS> <OBJECTS>'
    windows_extra_defines['CMAKE_CXX_ARCHIVE_APPEND'] = \
        '<CMAKE_AR> rsPD <TARGET> <LINK_FLAGS> <OBJECTS>'
    windows_extra_defines['CMAKE_C_ARCHIVE_APPEND'] = \
        '<CMAKE_AR> rsPD <TARGET> <LINK_FLAGS> <OBJECTS>'

    cflags = []
    cxxflags = []
    ldflags = []

    if is_32_bit:
        cflags.append('-m32')
        cxxflags.append('-m32')
        ldflags.append('-m32')

        # 32-bit libraries belong in lib/.
        windows_extra_defines['LLVM_LIBDIR_SUFFIX'] = ''

    windows_extra_defines['CMAKE_C_FLAGS'] = ' '.join(cflags)
    windows_extra_defines['CMAKE_CXX_FLAGS'] = ' '.join(cxxflags)
    windows_extra_defines['CMAKE_EXE_LINKER_FLAGS'] = ' '.join(ldflags)
    windows_extra_defines['CMAKE_SHARED_LINKER_FLAGS'] = ' '.join(ldflags)
    windows_extra_defines['CMAKE_MODULE_LINKER_FLAGS'] = ' '.join(ldflags)

    build_llvm(targets=targets, build_dir=build_dir, install_dir=install_dir,
               extra_defines=windows_extra_defines)


def build_stage1(stage1_install):
    # Build/install the stage 1 toolchain
    stage1_path = utils.android_path('out', 'stage1')
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


def build_stage2(stage1_install, stage2_install, stage2_targets, use_lld=False):
    # TODO(srhines): Build LTO plugin (Chromium folks say ~10% perf speedup)

    # Build/install the stage2 toolchain
    stage2_cc = os.path.join(stage1_install, 'bin', 'clang')
    stage2_cxx = os.path.join(stage1_install, 'bin', 'clang++')
    stage2_path = utils.android_path('out', 'stage2')

    stage2_extra_defines = dict()
    stage2_extra_defines['CMAKE_C_COMPILER'] = stage2_cc
    stage2_extra_defines['CMAKE_CXX_COMPILER'] = stage2_cxx
    stage2_extra_defines['LLVM_BUILD_RUNTIME'] = 'ON'
    stage2_extra_defines['LLVM_ENABLE_LIBCXX'] = 'ON'

    if use_lld:
        stage2_extra_defines['LLVM_ENABLE_LLD'] = 'ON'

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


def build_runtimes(stage2_install):
    version = extract_clang_version(stage2_install)
    build_crts(stage2_install, version)
    build_libfuzzers(stage2_install, version)
    # Bug: http://b/64037266. `strtod_l` is missing in NDK r15. This will break
    # libcxx build.
    # build_libcxx(stage2_install, version)
    build_asan_test(stage2_install)


def install_winpthreads(is_windows32, install_dir):
    """Installs the winpthreads runtime to the Windows bin directory."""
    lib_name = 'libwinpthread-1.dll'
    mingw_dir = utils.android_path(
        'prebuilts/gcc/linux-x86/host/x86_64-w64-mingw32-4.8',
        'x86_64-w64-mingw32')
    # Yes, this indeed may be found in bin/ because the executables are the
    # 64-bit version by default.
    pthread_dir = 'lib32' if is_windows32 else 'bin'
    lib_path = os.path.join(mingw_dir, pthread_dir, lib_name)

    lib_install = os.path.join(install_dir, 'bin', lib_name)
    install_file(lib_path, lib_install)


def remove_static_libraries(static_lib_dir):
    if os.path.isdir(static_lib_dir):
        lib_files = os.listdir(static_lib_dir)
        for lib_file in lib_files:
            if lib_file.endswith('.a'):
                static_library = os.path.join(static_lib_dir, lib_file)
                remove(static_library)


def package_toolchain(build_dir, build_name, host, dist_dir, strip=True):
    is_windows32 = host == 'windows-i386'
    is_windows64 = host == 'windows-x86'
    is_windows = is_windows32 or is_windows64
    is_linux = host == 'linux-x86'
    package_name = 'clang-' + build_name
    install_host_dir = utils.android_path('out', 'install', host)
    install_dir = os.path.join(install_host_dir, package_name)

    # Remove any previously installed toolchain so it doesn't pollute the
    # build.
    if os.path.exists(install_host_dir):
        shutil.rmtree(install_host_dir)

    # First copy over the entire set of output objects.
    shutil.copytree(build_dir, install_dir, symlinks=True)

    ext = '.exe' if is_windows else ''
    shlib_ext = '.dll' if is_windows else '.so' if is_linux else '.dylib'

    # Next, we remove unnecessary binaries.
    necessary_bin_files = [
            'clang' + ext,
            'clang++' + ext,
            'clang-5.0' + ext,
            'clang-format' + ext,
            'clang-tidy' + ext,
            'git-clang-format',  # No extension here
            'llvm-ar' + ext,
            'llvm-as' + ext,
            'llvm-dis' + ext,
            'llvm-link' + ext,
            'llvm-profdata' + ext,
            'llvm-symbolizer' + ext,
            'sancov' + ext,
            'sanstats' + ext,
            'LLVMgold' + shlib_ext,
            ]
    bin_dir = os.path.join(install_dir, 'bin')
    bin_files = os.listdir(bin_dir)
    for bin_filename in bin_files:
        binary = os.path.join(bin_dir, bin_filename)
        if os.path.isfile(binary):
            if bin_filename not in necessary_bin_files:
                remove(binary)
            elif strip:
                if not bin_filename.startswith('git-clang-format'):
                    check_call(['strip', binary])

    # TODO(srhines): Add/install the compiler wrappers.

    # Next, we remove unnecessary static libraries.
    if is_windows32:
        remove_static_libraries(os.path.join(install_dir, 'lib'))
    else:
        remove_static_libraries(os.path.join(install_dir, 'lib64'))

    # For Windows, add other relevant libraries.
    if is_windows:
        install_winpthreads(is_windows32, install_dir)

    # Add an AndroidVersion.txt file.
    version = extract_clang_version(build_dir)
    version_file_path = os.path.join(install_dir, 'AndroidVersion.txt')
    with open(version_file_path, 'w') as version_file:
        version_file.write('{}\n'.format(version.long_version()))

    # Package up the resulting trimmed install/ directory.
    tarball_name = package_name + '-' + host
    package_path = os.path.join(dist_dir, tarball_name) + '.tar.bz2'
    logger().info('Packaging %s', package_path)
    args = [
        'tar', '-cjC', install_host_dir, '-f', package_path, package_name
    ]
    check_call(args)


def parse_args():
    """Parses and returns command line arguments."""
    parser = argparse.ArgumentParser()

    parser.add_argument(
        '-v', '--verbose', action='count', default=0,
        help='Increase log level. Defaults to logging.INFO.')
    parser.add_argument(
        '--build-name', default='dev', help='Release name for the package.')

    parser.add_argument('--use-lld', action='store_true', default=False,
                        help='Use lld for linking (only affects stage2)')

    # Options to skip build or packaging (can't skip both, or the script does
    # nothing).
    build_package_group = parser.add_mutually_exclusive_group()
    build_package_group.add_argument('--skip-build', '-sb',
        action='store_true', default=False,
        help='Skip the build, and only do the packaging step')
    build_package_group.add_argument('--skip-package', '-sp',
        action='store_true', default=False,
        help='Skip the packaging, and only do the build step')

    parser.add_argument('--no-strip', action='store_true',
                        default=False, help='Don\t strip binaries/libraries')

    return parser.parse_args()


def main():
    args = parse_args()
    do_build = not args.skip_build
    do_package = not args.skip_package
    do_strip = not args.no_strip

    log_levels = [logging.INFO, logging.DEBUG]
    verbosity = min(args.verbose, len(log_levels) - 1)
    log_level = log_levels[verbosity]
    logging.basicConfig(level=log_level)

    stage1_install = utils.android_path('out', 'stage1-install')
    stage2_install = utils.android_path('out', 'stage2-install')
    windows32_install = utils.android_path('out', 'windows-i386-install')
    windows64_install = utils.android_path('out', 'windows-x86-install')

    # TODO(pirama): Once we have a set of prebuilts with lld, pass use_lld for
    # stage1 as well.
    if do_build:
        build_stage1(stage1_install)
        build_stage2(stage1_install, stage2_install, STAGE2_TARGETS,
                     args.use_lld)

    if do_build and utils.build_os_type() == 'linux-x86':
        build_runtimes(stage2_install)

        # Build single-stage clang for Windows
        windows_targets = STAGE2_TARGETS

        # Build 64-bit clang for Windows
        windows64_path = utils.android_path('out', 'windows-x86')
        build_llvm_for_windows(targets=windows_targets,
                               build_dir=windows64_path,
                               install_dir=windows64_install,
                               native_clang_install=stage2_install)

        # Build 32-bit clang for Windows
        windows32_path = utils.android_path('out', 'windows-i386')
        build_llvm_for_windows(targets=windows_targets,
                               build_dir=windows32_path,
                               install_dir=windows32_install,
                               native_clang_install=stage2_install,
                               is_32_bit=True)

    if do_package:
        # TODO(srhines): This only packages builds for the host OS. It needs
        # to be extended to package up the Windows build as well.
        dist_dir = ORIG_ENV.get('DIST_DIR', utils.android_path('out'))
        package_toolchain(stage2_install, args.build_name,
                          utils.build_os_type(), dist_dir, strip=do_strip)

        if utils.build_os_type() == 'linux-x86':
            package_toolchain(windows32_install, args.build_name,
                              'windows-i386', dist_dir, strip=do_strip)
            package_toolchain(windows64_install, args.build_name,
                              'windows-x86', dist_dir, strip=do_strip)

    return 0


if __name__ == '__main__':
    main()
