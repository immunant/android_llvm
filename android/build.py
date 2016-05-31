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

#import version


THIS_DIR = os.path.realpath(os.path.dirname(__file__))
ORIG_ENV = dict(os.environ)

def android_path(*args):
    return os.path.realpath(os.path.join(THIS_DIR, '../../', *args))


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


def build_llvm(out_path, defines, env):
    flags = ['-G', 'Ninja']
    for key in defines:
        newdef = '-D' + key + '=' + defines[key]
        flags += [newdef]
    llvm_root_path = android_path('llvm')
    flags += [llvm_root_path]

    check_create_path(out_path)
    os.chdir(out_path)
    # TODO(srhines): Enable this with a flag, because it forces clean builds
    # due to the updated cmake generated files.
    #rm_cmake_cache(out_path)

    subprocess.check_call(
        ['cmake'] + flags, cwd=out_path, env=env)
    subprocess.check_call(
        ['ninja'], cwd=out_path, env=env)
    subprocess.check_call(
        ['ninja', 'install'], cwd=out_path, env=env)


def main():
    cflags = []
    cxxflags = []
    ldflags = []

    base_cmake_defines = {}

    base_cmake_defines['CMAKE_BUILD_TYPE'] = 'Release'
    base_cmake_defines['LLVM_ENABLE_ASSERTIONS'] = 'ON'
    base_cmake_defines['LLVM_ENABLE_THREADS'] = 'OFF'
    base_cmake_defines['LLVM_ENABLE_TIMESTAMPS'] = 'OFF'
    base_cmake_defines['LLVM_USE_CRT_RELEASE'] = 'MT'
    base_cmake_defines['CLANG_VENDOR'] = 'Android '

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

    stage1_env = dict(ORIG_ENV)

    # Build/install the stage 1 toolchain
    build_llvm(out_path=stage1_path, defines=stage1_defines, env=stage1_env)

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

    stage2_env = dict(ORIG_ENV)

    build_llvm(out_path=stage2_path, defines=stage2_defines, env=stage2_env)

    return 0


if __name__ == '__main__':
    main()