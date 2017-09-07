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

import os
import shutil
import stat
import sys

THIS_DIR = os.path.realpath(os.path.dirname(__file__))


def remove(path):
    if os.path.islink(path):
        os.unlink(path)
    elif os.path.isfile(path):
        os.remove(path)
    elif os.path.isdir(path):
        rm_tree(path)


def rm_tree(dir):

    def chmod_and_retry(func, path, _):
        if not os.access(path, os.W_OK):
            os.chmod(path, stat.S_IWUSR)
            return func(path)
        raise

    shutil.rmtree(dir, onerror=chmod_and_retry)


def android_path(*args):
    return os.path.realpath(os.path.join(THIS_DIR, '../../../', *args))


def llvm_path(*args):
    return os.path.realpath(os.path.join(THIS_DIR, '../', *args))


def build_os_type():
    if sys.platform.startswith('linux'):
        return 'linux-x86'
    else:
        return 'darwin-x86'


def host_is_linux():
    return build_os_type() == 'linux-x86'


def host_is_darwin():
    return build_os_type() == 'darwin-x86'
