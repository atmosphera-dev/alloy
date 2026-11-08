# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""GDB-only native completion observer for the approved profile workspace.

No inferior calls, application-state writes or product instrumentation. Finish breakpoints
observe the original parent's startup completion, PreferredApps write reply and
blob-limit setup. The shared probe then awaits the C consumer's explicit native
control-persistence fence before baseline capture and no-disk context creation.
The gesture observer acknowledges browser-native activation before fixture actions.
"""
import json
import os
from pathlib import Path
import sys
import time

import gdb

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from workspace import temp_path, validate_workspace, workspace_path
from profile_gesture_observer import observe_native_gestures
REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)
validate_workspace(WORK, REPO)
assert os.environ['TMPDIR'] == str(temp_path(WORK))
receipt = Path(os.environ['ALLOY_PROFILE_READY'])
assert receipt.is_relative_to(WORK / 'evidence')
completed = {}
destroyed = {}
entries = []
browser_inferior = gdb.selected_inferior()
symbol_source = json.loads(Path(os.environ['ALLOY_PROFILE_SYMBOLS']).read_text())
progress = dict(objects=[], installed={}, entries={}, returns={})
popup_completions = []
observe_lifetime = os.environ.get('ALLOY_PROFILE_OBSERVE_LIFETIME') == '1'


def report():
    save(receipt.with_name('native-observer-progress.json'), progress)


def save(path, value):
    temporary = path.with_suffix('.new')
    temporary.write_text(json.dumps(value, indent=2)+'\n')
    temporary.replace(path)


class Completed(gdb.FinishBreakpoint):
    def __init__(self, name, phase):
        super().__init__(gdb.newest_frame(), internal=True)
        self.name = name
        self.phase = phase

    def stop(self):
        observation = dict(time=time.time(), thread=gdb.selected_thread().global_num)
        if self.phase == 'destroyed':
            destroyed.setdefault(self.name, []).append(observation)
            progress['returns'] = destroyed
            report()
            stopped = receipt.with_name('native-context-stopped.json')
            if (len(destroyed.get('request-context', [])) >= 2 and
                    destroyed.get('browser-context') and destroyed.get('otr-profile') and
                    (observe_lifetime or not stopped.exists())):
                save(stopped, dict(pid=gdb.selected_inferior().pid, completed=destroyed,
                     scope='Two request contexts, browser context and OTR profile destructed'))
                if not observe_lifetime:
                    for entry in entries:
                        if entry.phase == 'destroyed':
                            entry.enabled = False
            return False
        completed.setdefault(self.name, observation)
        if len(completed) == 3 and not receipt.exists():
            save(receipt, dict(pid=gdb.selected_inferior().pid, completed=completed,
                scope='Native parent startup callbacks; control persistence fence still required'))
            for entry in entries:
                if entry.phase == 'startup':
                    entry.enabled = False
        return False


class Observe(gdb.Breakpoint):
    def __init__(self, symbol, name, phase='startup'):
        super().__init__(symbol, internal=True)
        self.name = name
        self.phase = phase

    def stop(self):
        if self.phase == 'destroyed':
            progress['entries'].setdefault(self.name, []).append(dict(
                time=time.time(), pc=str(gdb.parse_and_eval('$pc'))))
            report()
        if self.phase == 'destroyed' or self.name not in completed:
            Completed(self.name, self.phase)
        return False


class PopupCompleted(gdb.FinishBreakpoint):
    def __init__(self):
        super().__init__(gdb.newest_frame(), internal=True)

    def stop(self):
        popup_completions.append(dict(time=time.time(), thread=gdb.selected_thread().global_num))
        save(receipt.with_name('native-popup-completed.json'),
             dict(completed=popup_completions, scope='Native popup Show returned; no popup display or acceptance inferred'))
        return False


class ObservePopup(gdb.Breakpoint):
    def stop(self):
        PopupCompleted()
        return False


def exited(event):
    # GDB's own exit code does not necessarily equal the inferior's exit code.
    if event.inferior != browser_inferior:
        return
    save(receipt.with_name('native-exit.json'),
         dict(exit_code=getattr(event, 'exit_code', None)))


for command in ('set pagination off', 'set confirm off', 'set auto-load off',
                'set disable-randomization off', 'set breakpoint pending on',
                'set detach-on-fork on', 'set follow-fork-mode parent',
                'set print thread-events off', 'handle SIGPIPE nostop noprint pass',
                'handle SIGSYS nostop noprint pass'):
    gdb.execute(command)
for name, value in json.loads(os.environ['ALLOY_PROFILE_BROWSER_ENV']).items():
    assert '\n' not in value
    gdb.execute('set environment '+name+' = '+value)
for symbol, name in (
    ("'(anonymous namespace)::SetBrowserStartupIsComplete(StartupIsCompleteReason)'", 'startup'),
    ('apps::PreferredAppsImpl::WriteCompleted()', 'preferred-apps-write'),
    ('storage::BlobMemoryController::OnStorageLimitsCalculated(storage::BlobStorageLimits)',
     'blob-limit-setup')):
    entries.append(Observe(symbol, name))
ObservePopup('autofill::AutofillPopupControllerImpl::Show', internal=True)
gesture_observer = observe_native_gestures(receipt.with_name('native-gesture.json'), save)
def library_loaded(event):
    progress['objects'].append(event.new_objfile.filename)
    report()
    if Path(event.new_objfile.filename).resolve() != Path(symbol_source['library']):
        return
    # GDB canonicalizes mangled destructor lookups to a demangled overload.
    # Use the exact fresh ELF destructor-body offsets with the loader's
    # offset-zero mapping; no private C++ object layout or inferior call is used.
    pid = gdb.selected_inferior().pid
    maps = Path('/proc/'+str(pid)+'/maps').read_text().splitlines()
    bases = [int(fields[0].split('-')[0], 16) for line in maps
             for fields in [line.split(None, 5)] if len(fields) == 6 and
             int(fields[2], 16) == 0 and fields[5] == symbol_source['library']]
    assert len(bases) == 1
    for name, offset in symbol_source['destructors'].items():
        address = hex(bases[0]+offset)
        entries.append(Observe('*'+address, name, 'destroyed'))
        progress['installed'][name] = address
    report()


gdb.events.new_objfile.connect(library_loaded)
gdb.events.exited.connect(exited)
