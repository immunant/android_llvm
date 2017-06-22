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

import errno
import fcntl
import os
import subprocess
import sys
import time

STDERR_REDIRECT_KEY = 'ANDROID_LLVM_STDERR_REDIRECT'
PREBUILT_COMPILER_PATH_KEY = 'ANDROID_LLVM_PREBUILT_COMPILER_PATH'

# We may introduce some new warnings after rebasing and we need to disable
# them before we fix those warnings.
DISABLED_WARNINGS = ['-Wno-error=zero-as-null-pointer-constant',
                     '-Wno-error=unknown-warning-option']

def real_compiler_path():
    return os.path.realpath(__file__) + '.real'


def fallback_compiler_path():
    return os.path.join(os.environ[PREBUILT_COMPILER_PATH_KEY],
                        os.path.basename(__file__))


def write_log(path, command, log):
    with open(path, 'a+') as f:
        while True:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except IOError as e:
                if e.errno == errno.EAGAIN or e.errno == errno.EACCES:
                    time.sleep(0.5)
        f.write('==================COMMAND:====================\n')
        f.write(' '.join(command) + '\n\n')
        f.write(log)
        f.write('==============================================\n\n')


def exec_clang(redirect_path, argv):
    command = [real_compiler_path()] + argv[1:] + ["-fno-color-diagnostics"] +\
              DISABLED_WARNINGS
    p = subprocess.Popen(command,
                         stderr=subprocess.PIPE)
    (_, err) = p.communicate()
    if len(err) > 0:
        write_log(redirect_path, command, err)
    if p.returncode != 0:
        os.execv(fallback_compiler_path(), argv)
    return p.returncode


def main(argv):
    if STDERR_REDIRECT_KEY in os.environ:
        redirect_path = os.environ[STDERR_REDIRECT_KEY]
        sys.exit(exec_clang(redirect_path, argv))
    else:
        os.execv(real_compiler_path(),
                 [argv[0] + '.real'] + argv[1:] + DISABLED_WARNINGS)


if __name__ == '__main__':
    main(sys.argv)
