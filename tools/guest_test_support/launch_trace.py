# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Fixture-only syscall launch observation, active before SDK startup."""
import ast
import os
from pathlib import Path
import re


class RuntimeLaunchTrace:
    def __init__(self, out, prefix):
        self.out, self.path = Path(out), Path(prefix)

    def command(self):
        # Keep Popen's PID as the SDK process so abort retains its normal
        # parent-death cleanup semantics, instead of killing only its tracer.
        return ['strace', '-D', '-ff', '-qq', '-xx', '-s', '4096', '-e', 'trace=execve', '-o', str(self.path)]

    def launches(self):
        files = sorted(self.path.parent.glob(self.path.name + '.*'))
        assert files, 'No launch trace was collected'
        sdk = False
        entries = []
        for path in files:
            for line in path.read_text(encoding='ascii').splitlines():
                if 'execve' not in line:
                    continue
                match = re.match(r'execve\(("(?:[^"\\]|\\.)*"), (\[.*\]), ', line)
                assert match, ('Incomplete launch observation', path, line)
                executable = os.fsdecode(ast.literal_eval('b' + match[1]))
                token = r'"(?:[^"\\]|\\.)*"'
                remainder = re.sub(token, '', match[2])
                assert not remainder.strip('[], \t'), ('Truncated argument vector', path, line)
                arguments = [os.fsdecode(ast.literal_eval('b' + value))
                             for value in re.findall(token, match[2])]
                sdk |= executable == str(self.out / 'alloy_guest_sdk_consumer')
                if executable not in {str(self.out / name) for name in ('alloy_guest_host', 'alloy_guest_engine')}:
                    continue
                if executable == str(self.out / 'alloy_guest_host') and arguments in (
                        [executable, '--alloy-wait-owner'], [executable, '--alloy-teardown-probe']):
                    continue
                entries.append(dict(executable=executable, arguments=arguments, trace=str(path)))
        assert sdk, 'Trace did not cover SDK process startup'
        return entries
