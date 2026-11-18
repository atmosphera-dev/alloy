#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Local Flatpak/native acceptance and startup refusal; Snap guest mode is unsupported."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import queue
import re
import signal
import subprocess
import time

from workspace import validate_output, workspace_path
from guest_test_support.fixtures import Fixtures
from guest_packaging_support import LocalFlatpak, PackageMembers
from guest_lock_probe import inventory, inventory_equal, package_proof
from guest_lock_probe import native_writers, defaults, lifecycle, mode_and_app, pressure
from guest_session_probe import smoke, blocking, recovery, close_and_observe
from guest_test_support.session import (Client, OwnedProcesses, descendants,
                                       kill_renderers, navigate_after_crash,
                                       assert_navigation_outcomes)

REPO = Path(__file__).resolve().parents[1]
WORK = workspace_path(REPO)


def protected(report):
    assert report['filesystem'] == 'data-writes-denied', report
    assert report['scratch'] == 'verified-tmpfs', report
    coverage = json.loads(report['coverage'])
    native = json.loads(report['sandbox'])
    limits = json.loads(report['limits'])
    assert native['route'] == 'flatpak-brokered-userns', native
    assert native['browser']['no_new_privs'], native
    assert native['renderer']['observed_after_blank_load'], native
    assert native['renderer']['no_new_privs'] and native['renderer']['seccomp'] == 2, native
    assert native['renderer']['effective_capabilities_decimal'] == '0', native
    assert coverage['before_engine'], coverage
    assert coverage['flatpak_external_entries'].startswith('fixed-bootstrap-per-entry'), coverage
    assert coverage['snap_external_entries'].startswith('unsupported:'), coverage
    assert 'native-namespace-view-absent' in limits['namespace_filesystem_and_packages'], limits
    assert limits['metadata_and_atime'] == 'not-refused-by-Landlock', limits
    return report


def roles(rows):
    engines = [row for row in rows if Path(row['exe']).name == 'alloy_guest_engine']
    assert {'browser', 'zygote', 'renderer', 'gpu-process', 'utility'} <= {r['role'] for r in engines}, rows
    browser = next(row for row in engines if row['role'] == 'browser')
    for row in engines:
        assert row['status']['NoNewPrivs'] == '1', row
        assert int(row['status']['CapEff'], 16) == 0, row
        assert all(row['fds'].get(str(fd)) == '/dev/null' for fd in (0, 1, 2)), row
        assert not any(re.fullmatch(
            r'/run/user/\d+/alloy-guest/alloy-guest-\d+-[^/]+', target)
            for target in row['fds'].values()), row
        if row['role'] == 'renderer':
            assert row['status']['Seccomp'] == '2', row
            assert row['namespaces']['pid'] != browser['namespaces']['pid'], row
            assert row['namespaces']['mnt'] != browser['namespaces']['mnt'], row
    return rows


def close_package(client, _out=None):
    with PackageMembers(client.package.snapshot()) as members:
        assert any(row['role'] == 'supervisor' for row in members.rows), members.rows
        client.send('close guest')
        client.closed()
        members.exited()
        return members.rows


def feasibility(package, evidence, fixtures):
    with package.client(evidence / 'client', trusted_certificate=fixtures.certificate) as client:
        report = protected(client.start(required='data-writes-denied'))
        client.active()
        page = client.page(fixtures.base + '/smoke', 'Internet - Alloy smoke fixture')
        load = client.wait('guest', 'load', lambda value: value['view'] == page['view'] and value['status'] == 200)
        observed = roles(package.snapshot())
        exited = close_package(client)
        client.finish()
        return [dict(case='real-Flatpak-CEF-page-and-external-sandbox', report=report,
                     page=page, load=load, processes=observed, exited=exited)]


def defaults_and_death(package, evidence, fixtures):
    rows = []
    app_data = Path.home() / '.var/app' / package.app_id
    # Initialize Flatpak's own package/runtime directories without loading Alloy
    # or warming the guest's Fontconfig/NSS stores. Inventory covers Alloy cold
    # startup, navigation, live state and teardown, not package installation.
    package.run(['flatpak', 'run', '--user', '--command=/usr/bin/true', package.app_id])
    baseline = inventory([app_data])
    for death in ('close', 'browser', 'renderer', 'gpu-process', 'supervisor', 'app', 'scratch'):
        with package.client(evidence / death, trusted_certificate=fixtures.certificate) as client:
            report = protected(client.start(required='data-writes-denied'))
            inventory_equal(baseline, inventory([app_data]))
            client.active()
            page = client.page(fixtures.base + '/title/PACKAGED_COLD_HTTPS', 'PACKAGED_COLD_HTTPS')
            client.wait('guest', 'load', lambda value: value['view'] == page['view'] and value['status'] == 200)
            observed = roles(package.snapshot())
            inventory_equal(baseline, inventory([app_data]))
            with PackageMembers(observed) as members:
                navigation = None
                if death == 'close':
                    client.send('close guest')
                    client.closed()
                elif death == 'app':
                    apps = [row for row in observed if row['role'] == 'app']
                    assert len(apps) == 1, apps
                    handle = os.pidfd_open(apps[0]['pid'])
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGKILL)
                    finally:
                        os.close(handle)
                elif death == 'scratch':
                    roots = {Path(target) for row in observed if row['role'] == 'supervisor'
                             for target in row['fds'].values() if re.fullmatch(
                                 r'/run/user/\d+/alloy-guest/alloy-guest-\d+-[^/]+', target)}
                    assert len(roots) == 1, roots
                    roots.pop().chmod(0o500)
                    client.closed(reason='failed')
                else:
                    members.kill(death)
                    if death in ('renderer', 'gpu-process'):
                        navigation = navigate_after_crash(client, page['view'],
                            fixtures.base + '/title/PACKAGED_RECOVERED', 'PACKAGED_RECOVERED')
                        resumed = navigation['loaded']
                        assert resumed['document'] > page['document'], resumed
                        roles(package.snapshot())
                        close_package(client)
                    else:
                        client.closed(reason='failed')
                members.exited()
            if death == 'app':
                client.abort()
            else:
                client.finish()
            if navigation is not None:
                assert_navigation_outcomes(client, navigation)
            inventory_equal(baseline, inventory([app_data]))
            rows.append(dict(case='cold-defaults-and-' + death, report=report, page=page,
                             processes=observed, durable_changes=0))
    return rows


def flexible_app(package, evidence, fixtures):
    with package.client(evidence / 'client', encrypted_app=True) as client:
        client.wait('app', 'initialized')
        client.send('app ' + fixtures.base + '/title/FLEXIBLE_BEFORE')
        client.wait('app', 'title', lambda value: value['title'] == 'FLEXIBLE_BEFORE')
        report = protected(client.start(required='data-writes-denied'))
        client.active()
        client.page(fixtures.base + '/title/PROTECTED_WITH_FLEXIBLE_APP', 'PROTECTED_WITH_FLEXIBLE_APP')
        close_package(client)
        client.send('app ' + fixtures.base + '/title/FLEXIBLE_AFTER')
        page = client.wait('app', 'title', lambda value: value['title'] == 'FLEXIBLE_AFTER')
        client.send('app-lock')
        assert client.wait('app', 'locked')['ok']
        client.finish()
        return [dict(case='flexible-encrypted-app-survives-guest', report=report, page=page)]


def admission_and_modes(package, evidence, fixtures):
    rows = []
    with package.client(evidence / 'stateless') as client:
        report = protected(client.start(mode='stateless', required='data-writes-denied'))
        client.active()
        page = client.page(fixtures.base + '/set-cookie', 'COOKIE_REFUSED')
        close_package(client)
        rows.append(dict(case='packaged-stateless-cookie-refusal', report=report, page=page))
    for policy, required, reason in (('auto', 'read-only-view', 'refused'),
                                     ('queue-close', 'data-writes-denied', 'closed')):
        with package.client(evidence / policy) as client:
            report = protected(client.start(policy=policy, required=required))
            client.closed(reason=reason)
            client.finish()
            assert not any(event['event'] == 'view-ready' for event in client.events)
            assert not [row for row in package.snapshot() if row['role'] != 'app']
            rows.append(dict(case='packaged-admission-' + policy, report=report, reason=reason))
    return rows


def missing_ram_permission(package, evidence):
    # An override can leave a private tmpfs placeholder which the FD portal can
    # still transfer. Remove the carrier from the real manifest and prove it is
    # unavailable, rather than mistaking removal of a host bind for loss of RAM.
    with package.without_permission('Context', 'filesystems', 'xdg-run/alloy-guest:create') as denied:
        denied.run(['flatpak', 'run', '--user', '--command=test', denied.app_id,
                     '!', '-e', f'/run/user/{os.getuid()}/alloy-guest'])
        with denied.client(evidence / 'client') as client:
            client.send('start guest auto memory data-writes-denied 0 -')
            client.closed(reason='failed')
            client.finish()
            assert not any(event['event'] in ('prepared', 'view-ready') or event.get('state') == 'ACTIVE'
                           for event in client.events), client.events
            assert not [row for row in denied.snapshot() if row['role'] != 'app']
            return [dict(case='missing-shared-RAM-permission-refuses-before-engine', failed_closed=True)]


def missing_launch_ipc(package, evidence):
    # Flatpak always permits calls to portal names. Removing an explicit talk
    # policy does not remove this prerequisite; omit the actual session socket.
    package.run(['flatpak', 'run', '--user', '--no-session-bus', '--command=test',
                 package.app_id, '!', '-S', '/run/flatpak/bus'])
    client = package.client(evidence / 'client', session_bus=False)
    try:
        try:
            client.send('start guest auto memory data-writes-denied 0 -')
        except BrokenPipeError:
            pass  # The packaging bridge can refuse before the SDK starts.
        deadline = time.monotonic() + 120
        while True:
            try:
                event = client.incoming.get(timeout=max(0, deadline - time.monotonic()))
            except queue.Empty:
                raise AssertionError('Missing launch IPC produced no failure outcome')
            if event is None:
                code = client.process.wait(timeout=30)
                assert code != 0, 'Launch bridge exited successfully without a guest outcome'
                outcome = dict(process_exit=code)
                break
            client.events.append(event)
            assert event['event'] not in ('prepared', 'view-ready') and event.get('state') != 'ACTIVE', event
            if event['event'] == 'closed':
                assert event['reason'] == 'failed', event
                client.finish()
                outcome = event
                break
        assert not package.snapshot(), 'Missing portal access left package runtime entries'
        return [dict(case='missing-Flatpak-launch-IPC-refuses-before-engine', outcome=outcome)]
    finally:
        if not client.finished:
            client.abort()


def missing_helper(package, evidence):
    with package.missing_asset('zypak/lib/libzypak-preload-child.so'):
        with package.client(evidence / 'client') as client:
            client.send('start guest auto memory data-writes-denied 0 -')
            client.closed(reason='failed')
            client.finish()
            assert not any(event['event'] in ('prepared', 'view-ready') for event in client.events)
            assert not package.snapshot(), 'Incomplete package left runtime entries'
    return [dict(case='missing-pinned-entry-helper-refuses-before-engine', failed_closed=True)]


def native_acceptance(out, evidence, fixtures):
    cases = teardown_refusal(out, evidence / 'teardown', fixtures)
    with Fixtures(REPO, tls_root=evidence / 'native-tls') as tls:
        cases += native_writers(out, evidence, tls) + defaults(out, evidence, tls)
    cases += lifecycle(out, evidence, fixtures)
    cases += mode_and_app(out, evidence, fixtures)
    cases += pressure(out, evidence, fixtures)
    cases += pressure(out, evidence, fixtures, inodes=True)
    cases += package_proof(out, evidence, fixtures, name='alloy-gm-m7-native-fixture')
    return cases


def teardown_refusal(out, evidence, fixtures, filters=('deny', 'deny-kill')):
    evidence.mkdir(parents=True, exist_ok=True)
    wrapper = evidence / 'pidfd-signal-policy'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        REPO / 'tools/testdata/guest-packaging/deny_pidfd_signal.c', '-o', wrapper], check=True)
    checked_teardown = evidence / 'checked-supervisor-teardown'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', REPO / 'src', '-I', WORK / 'src',
        REPO / 'tools/testdata/guest-packaging/teardown_policy.c',
        *[REPO / 'src/brave/alloy/guest/linux_lock' / (name + '.c')
          for name in ('ram_scratch', 'child_launch')],
        REPO / 'src/brave/alloy/guest/namespace_lock/flatpak_entry.c',
        '-o', checked_teardown], check=True)
    with (evidence / 'checked-supervisor-teardown.log').open('x') as log:
        subprocess.run([checked_teardown], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
    assert 'GUEST_TEARDOWN_ERROR_REPORTED=1' in (evidence / 'checked-supervisor-teardown.log').read_text()
    rows = [dict(case='supervisor-signaling-failure-is-fatal', checked=True)]
    for policy, developmental in ((policy, developmental) for policy in filters
                                  for developmental in (False, True)):
        label = policy + ('-developmental' if developmental else '-required')
        with Client(out, evidence / label, launch_prefix=[wrapper, policy],
                    launch_environment={k: v for k, v in os.environ.items()
                                        if not k.startswith('SNAP')}) as client:
            client.send('start guest auto memory ' +
                        ('unavailable 1 -' if developmental else 'data-writes-denied 0 -'))
            report = client.wait('guest', 'prepared')
            processes = OwnedProcesses(client.process.pid, out)
            try:
                assert report['filesystem'] == 'unavailable', report
                assert report['limits'] == ('process policy denies pidfd_send_signal: '
                                            'identity-bound process teardown unavailable'), report
                assert report['scratch'] == 'unavailable' and report['extensions_unavailable'], report
                assert not report['stream_available'], report
                assert json.loads(report['coverage'])['runtime_supervision'] is False, report
                assert not client.wait('guest', 'accept-queued')['ok']
                client.closed(reason='failed')
                client.send('create guest ' + fixtures.base + '/title/NOT_ADMITTED')
                assert not client.wait('guest', 'create-queued')['ok']
                client.finish()
                assert not processes.members and not descendants(client.process.pid), processes.members
                assert not any(v['event'] == 'view-ready' or v.get('state') == 'ACTIVE'
                               for v in client.events), client.events
                states = [v for v in client.events if v['name'] == 'guest' and v['event'] == 'state']
                assert all(v['state'] == v['observed_state'] for v in states), states
                assert 'PIDFD_SIGNAL_FILTER_DENIED=1' in (client.root / 'consumer.log').read_text()
                rows.append(dict(case='pidfd-signal-' + label, report=report,
                                 runtime_entries=0, navigations=0))
            finally:
                # The pre-correction red test can reach the old engine. Cleanup
                # uses identities pinned by the unfiltered harness, never a
                # product numeric-PID fallback or a relaxed guest policy.
                for handle in processes.handles:
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                processes.exited()
                processes.close()
    with Client(out, evidence / 'positive', launch_prefix=[wrapper, 'allow'],
                launch_environment=dict(os.environ, SNAP='fixture-marker-only')) as client:
        client.start(required='data-writes-denied')
        client.active()
        page = client.page(fixtures.base + '/title/TEARDOWN_POLICY_ALLOWED', 'TEARDOWN_POLICY_ALLOWED')
        client.wait('guest', 'load', lambda v: v['document'] == page['document'] and v['status'] == 200)
        close_and_observe(client, out)
        client.finish()
        rows.append(dict(case='pidfd-signal-allowed-loads', page=page))
    assert not any(v['path'] == '/title/NOT_ADMITTED' for v in fixtures.requests)
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', choices=('feasibility', 'teardown', 'teardown-kill', 'recovery', 'flatpak', 'native', 'all'), required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args(argv)
    out = args.out.resolve()
    validate_output(WORK, out, REPO)
    if os.uname().nodename != 'alloy-gm-m7' or os.getuid() == 0:
        parser.error('Run unprivileged inside the assigned alloy-gm-m7 cloud guest')
    if args.check == 'all':
        parser.error('Snap guest mode is unsupported; use flatpak/native/teardown acceptance')
    evidence = args.evidence.resolve()
    if not evidence.is_relative_to(WORK) or evidence.is_relative_to(Path('/tmp')):
        parser.error('Evidence must remain in the assigned disk-backed workspace')
    evidence.mkdir(mode=0o700, parents=True, exist_ok=False)
    started = time.monotonic()
    result = dict(check=args.check, out=str(out), status='running', snap='unsupported')
    lock = (WORK / 'guest-packaging.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    try:
        full = args.check == 'flatpak'
        with Fixtures(REPO, tls_root=evidence / 'tls' if full or args.check == 'recovery' else None,
                      tracker_https=full) as fixtures:
            if args.check in ('teardown', 'teardown-kill'):
                cases = teardown_refusal(out, evidence, fixtures,
                    filters=('deny-kill',) if args.check == 'teardown-kill' else ('deny', 'deny-kill'))
            elif args.check == 'native':
                cases = native_acceptance(out, evidence, fixtures)
            else:
                with LocalFlatpak(REPO, out, evidence) as package:
                    cases = feasibility(package, evidence / 'feasibility', fixtures)
                    result['cases'] = cases
                    if full or args.check == 'recovery':
                        def interrupt(client, wait_for_exit):
                            with PackageMembers(package.snapshot()) as members:
                                return kill_renderers(members.handles, wait_for_exit)
                        cases += recovery(out, evidence / 'recovery', fixtures,
                            lambda _out, root, **kwargs: package.client(root,
                                trusted_certificate=fixtures.certificate, **kwargs),
                            interrupt, close_package, lambda client: package.snapshot())
                    if full:
                        cases += defaults_and_death(package, evidence / 'defaults', fixtures)
                        def factory(_out, root, **kwargs):
                            kwargs.setdefault('trusted_certificate', fixtures.certificate)
                            return package.client(root, **kwargs)
                        cases += smoke(out, evidence / 'smoke', fixtures, factory, close_package)
                        cases += blocking(out, evidence / 'blocking', fixtures, '/app/share/alloy-data',
                                          factory, close_package)
                        with Fixtures(REPO) as plain:
                            cases += admission_and_modes(package, evidence / 'admission', plain)
                            cases += flexible_app(package, evidence / 'flexible-app', plain)
                            (evidence / 'flexible-requests.json').write_text(
                                json.dumps(plain.requests, indent=2) + '\n')
                        cases += missing_ram_permission(package, evidence / 'missing-permission')
                        cases += missing_launch_ipc(package, evidence / 'missing-launch-IPC')
                        cases += missing_helper(package, evidence / 'missing-helper')
                    assert not [row for row in package.snapshot() if row['role'] != 'app'], 'Leaked package runtime'
            (evidence / 'fixture-requests.json').write_text(json.dumps(fixtures.requests, indent=2) + '\n')
        result.update(status='passed', cases=cases, seconds=time.monotonic() - started)
        marker = dict(feasibility='GUEST_FLATPAK_FEASIBILITY_PASS', recovery='GUEST_FLATPAK_RECOVERY_PASS',
                      flatpak='GUEST_FLATPAK_PASS',
                       native='GUEST_NATIVE_PACKAGE_PASS', teardown='GUEST_TEARDOWN_REFUSAL_PASS')[
                           'teardown' if args.check == 'teardown-kill' else args.check]
        print(marker, flush=True)
        return 0
    except BaseException as error:
        result.update(status='failed', error=repr(error), seconds=time.monotonic() - started)
        raise
    finally:
        (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        lock.close()


if __name__ == '__main__':
    raise SystemExit(main())
