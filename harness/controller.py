# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Fixed-duration collector. No evidence analysis runs in this process."""
from common import *
import hashlib
import re
import shutil
import signal
import traceback
import network


def health(d):
    sample = dict(time=now(), free_bytes=shutil.disk_usage(ROOT).free,
                  controller_state=pathlib.Path('/sys/class/nvme/nvme0/state').read_text().strip())
    sample['nvme_errors'] = [line for line in run(['dmesg']).stdout.splitlines()
                             if re.search(r'nvme.*(error|timeout|reset)', line, re.I)]
    sample['smart'] = json.loads(run(['nvme', 'smart-log', '/dev/nvme0', '-o', 'json'], timeout=15).stdout)
    sample['apst'] = pathlib.Path('/sys/module/nvme_core/parameters/default_ps_max_latency_us').read_text().strip()
    sample['trim'] = run(['systemctl', 'is-enabled', 'fstrim.timer', 'fstrim.service'], check=False).stdout.strip()
    baseline = json.loads((d / 'health-baseline.json').read_text()) if (d / 'health-baseline.json').exists() else sample
    errors = []
    if sample['controller_state'] != 'live' or sample['nvme_errors']:
        errors.append('NVMe controller incident; stop without reboot')
    if sample['free_bytes'] < 200_000_000_000:
        errors.append('free space below 200 GB')
    if sample['apst'] != '0' or sample['trim'] != 'masked\nmasked':
        errors.append('storage mitigations changed')
    for key in ('critical_warning', 'media_errors', 'num_err_log_entries'):
        if sample['smart'].get(key, 1) > baseline['smart'].get(key, 0) or (key == 'critical_warning' and sample['smart'].get(key)):
            errors.append('NVMe new ' + key)
    sizes = []
    for p in d.rglob('*'):
        if p.is_file() and not p.is_symlink():
            try:
                sizes.append(p.stat().st_size)
            except FileNotFoundError:
                pass  # Atomic status replacement, never a raw evidence deletion.
    sample['evidence_bytes'] = sum(sizes)
    if sum(sizes) > 100 * 1024**3 or any(size > 512 * 1024**2 for size in sizes):
        errors.append('evidence safety budget reached')
    sample['errors'] = errors
    Log(d / 'health.jsonl').event('health', sample=sample)
    save(d / 'health-latest.json', sample)
    if not (d / 'health-baseline.json').exists():
        save(d / 'health-baseline.json', sample)
    if errors:
        save(d / 'SAFETY-STOP.json', sample)
        raise RuntimeError(str(errors))


def lane_start(d, lane, runtime):
    directory = d / lane['name']
    props = dict(NetworkNamespacePath='/run/netns/' + lane['ns'], PrivateTmp='yes', KillMode='mixed',
                 BindReadOnlyPaths=f'/etc/netns/{lane["ns"]}/resolv.conf:/etc/resolv.conf /etc/netns/{lane["ns"]}/nsswitch.conf:/etc/nsswitch.conf',
                 InaccessiblePaths='-/run/user/1000/bus -/run/dbus/system_bus_socket',
                 StandardOutput='append:' + str(directory / 'unit.log'),
                 StandardError='append:' + str(directory / 'unit.log'))
    unit(d.name + '-' + lane['name'] + '-lane', ['python3', SCRIPTS / 'lane.py', lane['name'], directory], ROOT,
         properties=props, runtime=runtime, env=dict(HOME=str(directory / 'home'), DISPLAY=':99',
         DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent/alloy-harness-bus',
         XDG_CONFIG_HOME=str(directory / 'home/.config'), XDG_CACHE_HOME=str(directory / 'home/.cache'),
         SOAK_NETNS_INODE=str(pathlib.Path('/run/netns', lane['ns']).stat().st_ino),
         SOAK_CAMPAIGN_ORIGIN=str(CONFIG['campaign_origin'])))


def collect(d):
    guard()
    assert os.geteuid() == 0
    owner = lock(ROOT / 'controller.lock')
    log = Log(d / 'controller.jsonl')
    stopped = False
    def stop(*_):
        nonlocal stopped
        stopped = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    elapsed = {l['name']: 0.0 for l in LANES}
    target = CONFIG['target']
    begin = time.monotonic()
    last = begin
    seen = set()
    try:
        health(d)
        network.setup(d)
        network.services(d, target + 1800)
        network.seal_probe(d)
        time.sleep(3)
        network.nft_snapshot(d, 'baseline-nft.json')
        save(d / 'collection-start.json', dict(time=now(), epoch=time.time()))
        for lane in LANES:
            lane_start(d, lane, target + 1200)
        last_health = time.monotonic()
        while time.monotonic() - begin < target + 900:
            if stopped or (d / 'STOP').exists():
                raise RuntimeError('explicit or analysis stop requested')
            current = time.monotonic()
            delta = current - last
            last = current
            for lane in LANES:
                for role in ['dns', 'pcap'] + (['proxy'] if lane['mode'] == 'observe' else []):
                    assert state(d.name + '-' + lane['name'] + '-' + role)['ActiveState'] == 'active', 'provider failed ' + role
                s = state(d.name + '-' + lane['name'] + '-lane')
                assert s['ActiveState'] == 'active', ('lane failed; no automatic replay', lane, s)
                path = d / lane['name'] / 'status.json'
                status = json.loads(path.read_text()) if path.exists() else {}
                ready = status.get('pid') == int(s['MainPID']) and status.get('operation') != 'startup'
                if ready:
                    if lane['name'] in seen:
                        elapsed[lane['name']] += delta
                    seen.add(lane['name'])
                    assert time.time() - status['epoch'] < 420, 'walker heartbeat stale'
            for role in ('nflog', 'uplink'):
                assert state(d.name + '-' + role)['ActiveState'] == 'active', 'provider failed ' + role
            if current - last_health >= 300:
                health(d)
                last_health = current
            save(d / 'status.json', dict(time=now(), phase='running', elapsed=elapsed, target=target,
                 wall_seconds=current - begin, owner_pid=os.getpid()))
            if min(elapsed.values()) >= target:
                save(d / 'run-complete.json', dict(time=now(), elapsed=elapsed, target=target, mode=CONFIG['mode']))
                break
            time.sleep(5)
        else:
            raise RuntimeError('fixed deadline reached; never extend duration')
    except BaseException as exc:
        save(d / 'FAILED.json', dict(time=now(), error=repr(exc), traceback=traceback.format_exc(), elapsed=elapsed))
        log.event('fatal', error=repr(exc))
    finally:
        try:
            network.cleanup(d)
            health(d)
        except BaseException as exc:
            save(d / 'cleanup-failed.json', dict(time=now(), error=repr(exc), traceback=traceback.format_exc()))
        save(d / 'final-status.json', dict(time=now(), elapsed=elapsed, target=target,
             phase='failed' if (d / 'FAILED.json').exists() or (d / 'cleanup-failed.json').exists() else 'awaiting-final-analysis'))
        log.close()
    return int((d / 'FAILED.json').exists() or (d / 'cleanup-failed.json').exists())


if __name__ == '__main__':
    sys.exit(collect(pathlib.Path(sys.argv[1])))
