#!/usr/bin/env python
#
# Copyright (C) 2017 The Android Open Source Project
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

"""Update the prebuilt clang from the build server."""

import argparse
import inspect
import logging
import os
import shutil
import subprocess
import sys
import utils


BRANCH = 'aosp-llvm-toolchain'


def logger():
    """Returns the module level logger."""
    return logging.getLogger(__name__)


def unchecked_call(cmd, *args, **kwargs):
    """subprocess.call with logging."""
    logger().info('unchecked_call: %s', subprocess.list2cmdline(cmd))
    subprocess.call(cmd, *args, **kwargs)


def check_call(cmd, *args, **kwargs):
    """subprocess.check_call with logging."""
    logger().info('check_call: %s', subprocess.list2cmdline(cmd))
    subprocess.check_call(cmd, *args, **kwargs)


class ArgParser(argparse.ArgumentParser):
    def __init__(self):
        super(ArgParser, self).__init__(
            description=inspect.getdoc(sys.modules[__name__]))

        self.add_argument(
            'build', metavar='BUILD',
            help='Build number to pull from the build server.')

        self.add_argument(
            '-b', '--bug', type=int,
            help='Bug to reference in commit message.')

        self.add_argument(
            '--use-current-branch', action='store_true',
            help='Do not repo start a new branch for the update.')

        self.add_argument(
            '--skip-fetch',
            '-sf',
            action='store_true',
            default=False,
            help='Skip the fetch, and only do the extraction step')

        self.add_argument(
            '--skip-cleanup',
            '-sc',
            action='store_true',
            default=False,
            help='Skip the cleanup, and leave intermediate files')


def fetch_artifact(branch, target, build, pattern):
    fetch_artifact_path = '/google/data/ro/projects/android/fetch_artifact'
    cmd = [fetch_artifact_path, '--branch={}'.format(branch),
           '--target={}'.format(target), '--bid={}'.format(build), pattern]
    check_call(cmd)


def extract_package(package, install_dir):
    cmd = ['tar', 'xf', package, '-C', install_dir]
    check_call(cmd)


def update_clang(host, build_number, use_current_branch, download_dir, bug,
                 manifest):
    prebuilt_dir = utils.android_path('prebuilts/clang/host', host)
    os.chdir(prebuilt_dir)

    if not use_current_branch:
        branch_name = 'update-clang-{}'.format(build_number)
        unchecked_call(
            ['repo', 'abandon', branch_name, '.'])
        check_call(
            ['repo', 'start', branch_name, '.'])

    package = '{}/clang-{}-{}.tar.bz2'.format(download_dir, build_number, host)
    manifest_file = '{}/{}'.format(download_dir, manifest)

    install_subdir = 'clang-' + build_number
    extract_package(package, prebuilt_dir)
    shutil.copy(manifest_file, prebuilt_dir + '/' +  install_subdir)

    check_call(['git', 'add', install_subdir])

    version_file_path = os.path.join(install_subdir, 'AndroidVersion.txt')
    with open(version_file_path) as version_file:
        version = version_file.read().strip()

    message_lines = [
        'Update prebuilt Clang to build {}.'.format(build_number),
        '',
        'Built from version {}.'.format(version),
    ]
    if bug is not None:
        message_lines.append('')
        message_lines.append('Bug: http://b/{}'.format(bug))
    message_lines.append('Test: N/A')
    message = '\n'.join(message_lines)
    check_call(['git', 'commit', '-m', message])


def main():
    args = ArgParser().parse_args()
    logging.basicConfig(level=logging.INFO)

    do_fetch = not args.skip_fetch
    do_cleanup = not args.skip_cleanup

    download_dir = os.path.realpath('.download')
    if do_fetch:
        if os.path.isdir(download_dir):
            shutil.rmtree(download_dir)
        os.makedirs(download_dir)

    os.chdir(download_dir)

    targets = ['linux', 'darwin_mac']
    # Bug: http://b/65857918 tracks support for windows-x86_32 prebuilts.
    # hosts = ['darwin-x86', 'linux-x86', 'windows-x86', 'windows-x86_32']
    hosts = ['darwin-x86', 'linux-x86', 'windows-x86']
    clang_pattern = 'clang-*.tar.bz2'
    manifest = 'manifest_{}.xml'.format(args.build)
    branch = 'aosp-llvm-toolchain'

    try:
        if do_fetch:
            fetch_artifact(branch, targets[0], args.build, manifest)
            for target in targets:
                fetch_artifact(branch, target, args.build, clang_pattern)

        for host in hosts:
            update_clang(host, args.build, args.use_current_branch,
                         download_dir, args.bug, manifest)
    finally:
        if do_cleanup:
            shutil.rmtree(download_dir)

    return 0


if __name__ == '__main__':
    main()
