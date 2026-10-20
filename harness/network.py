# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Owned network lifecycle: count before NAT, strip through teardown, seal once."""
from common import *
import re
from analysis.dscp import classes

TABLE = 'alloy_harness'


def ipv4_settings():
    return dict(line.split(' = ', 1) for line in run(['sysctl', '-a'], check=False).stdout.splitlines()
                if line.startswith('net.ipv4.') and ' = ' in line)


def nft_snapshot(d, name):
    value = json.loads(run(['nft', '-j', 'list', 'table', 'inet', TABLE]).stdout)
    save(d / name, value)
    return value


def setup(d):
    guard()
    assert os.geteuid() == 0
    assert not run(['ip', 'netns', 'list']).stdout.strip(), 'Existing namespaces: inspect first'
    assert not run(['nft', 'list', 'ruleset']).stdout.strip(), 'Existing firewall: inspect first'
    assert not any(pathlib.Path('/etc/netns', l['ns']).exists() for l in LANES)
    ports = list(map(int, pathlib.Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split()))
    assert ports[1] < 62000, 'Reserved NAT range overlaps ephemeral ports'
    assert not re.search(r':62\d{3}\s', run(['ss', '-H', '-a', '-n', '-t', '-u']).stdout)
    addresses = json.loads(run(['ip', '-j', '-4', 'addr', 'show', 'dev', 'eno1']).stdout)
    nat = next(a['local'] for a in addresses[0]['addr_info'] if a['scope'] == 'global')
    original = run(['sysctl', '-n', 'net.ipv4.ip_forward']).stdout.strip()
    before = ipv4_settings()
    save(d / 'host-settings.json', dict(ip_forward=original, ipv4_before=before,
         resolver=pathlib.Path('/etc/resolv.conf').read_text(), namespaces='', nft=''))
    save(ROOT / 'network-owner.json', dict(directory=str(d), active=True, time=now()))
    save(d / 'nat-source.json', dict(ip=nat, first_port=62000, last_port=62999, ephemeral_range=ports))
    for l in LANES:
        lane = d / l['name']
        lane.mkdir()
        for name in ('home', 'captures'):
            (lane / name).mkdir()
        run(['chown', '-R', 'builder:builder', lane])
        run(['ip', 'netns', 'add', l['ns']])
        net = pathlib.Path('/etc/netns') / l['ns']
        net.mkdir(parents=True, exist_ok=False)
        (net / 'resolv.conf').write_text('nameserver ' + l['host'] + '\noptions timeout:2 attempts:2\n')
        (net / 'nsswitch.conf').write_text('passwd: files\ngroup: files\nshadow: files\nhosts: files dns\nnetworks: files\n')
        for name in ('resolv.conf', 'nsswitch.conf'):
            (net / name).chmod(0o644)
        run(['ip', 'link', 'add', l['veth'], 'type', 'veth', 'peer', 'name', l['peer']])
        run(['ip', 'link', 'set', l['peer'], 'netns', l['ns']])
        run(['ip', 'addr', 'add', l['host'] + '/30', 'dev', l['veth']])
        run(['ip', 'link', 'set', l['veth'], 'up'])
        for args in (['link', 'set', 'lo', 'up'], ['addr', 'add', l['guest'] + '/30', 'dev', l['peer']],
                     ['link', 'set', l['peer'], 'up'], ['route', 'add', 'default', 'via', l['host']]):
            run(['ip', 'netns', 'exec', l['ns'], 'ip', *args])
        run(['ip', 'netns', 'exec', l['ns'], 'sysctl', '-w',
             'net.ipv6.conf.all.disable_ipv6=1', 'net.ipv6.conf.default.disable_ipv6=1'])
        l['guest_mac'] = json.loads(run(['ip','netns','exec',l['ns'],'ip','-j','link','show',l['peer']]).stdout)[0]['address']
    rows = [f'table inet {TABLE} {{', 'chain count { type filter hook prerouting priority -160; policy accept;']
    for l in LANES:
        mapping=classes()
        for value, name in [(code,mapping.get(code,'unmarked' if code==0 else 'codepoint-'+str(code))) for code in range(64)]:
            for family, addr in [('ip', l['guest']), ('ip6', None)]:
                source = f'{family} saddr {addr} ' if addr else ''
                rows.append(f'iifname "{l["veth"]}" {source}{family} dscp {value} counter comment "class:{l["name"]}:{name}:{family}"')
    rows += ['}', 'chain strip { type filter hook postrouting priority -150; policy accept;']
    for l in LANES:
        for family in ('ip', 'ip6'):
            rows.append(f'iifname "{l["veth"]}" {family} dscp set 0 counter comment "strip:{l["name"]}:{family}"')
    rows += ['}', 'chain input { type filter hook input priority -20; policy accept;']
    rows += [f'iifname "{l["veth"]}" jump lane_{l["n"]}' for l in LANES]
    rows += ['}', 'chain forward { type filter hook forward priority -20; policy accept;']
    rows += [f'iifname "{l["veth"]}" jump lane_{l["n"]}' for l in LANES]
    interfaces = ', '.join('"' + l['veth'] + '"' for l in LANES)
    rows += [f'oifname {{ {interfaces} }} ct state established,related accept',
             f'oifname {{ {interfaces} }} counter log prefix "alloy-inbound " group 77 drop', '}',
             'chain prerouting { type nat hook prerouting priority dstnat; policy accept;']
    rows += [f'iifname "{l["veth"]}" tcp dport {{80,443}} counter redirect to :{l["port"]}' for l in LANES if l['mode'] == 'observe']
    rows += ['}', 'chain nat { type nat hook postrouting priority srcnat; policy accept;']
    for l in LANES:
        lo = 62000 + (l['n'] - 1) * 500
        for proto in ('tcp', 'udp'):
            rows.append(f'ip saddr {l["guest"]} oifname "eno1" ip protocol {proto} counter snat ip to {nat}:{lo}-{lo + 499}')
    rows += ['}']
    for l in LANES:
        prefix = 'alloy-' + l['name'] + ' '
        rows += [f'chain lane_{l["n"]} {{',
                 'ip daddr 10.4.20.237 tcp dport 8090 counter accept comment "kiwix"',
                 f'ip daddr {l["host"]} udp dport 53 counter accept comment "lane-dns"',
                 f'ip daddr {l["host"]} tcp dport 53 counter accept comment "lane-dns"',
                 f'udp dport 443 counter log prefix "{prefix}quic " group 77 drop',
                 f'udp dport {{53,853}} counter log prefix "{prefix}dns-bypass " group 77 drop',
                 f'tcp dport {{53,853}} counter log prefix "{prefix}dns-bypass " group 77 drop',
                 f'meta nfproto ipv6 counter log prefix "{prefix}ipv6 " group 77 drop']
        if l['mode'] == 'observe':
            rows += [f'ct status dnat tcp dport {l["port"]} counter log prefix "{prefix}proxy " group 77 accept',
                     f'ip daddr {{10.77.0.0/16,{nat}}} counter log prefix "{prefix}host-seal " group 77 drop',
                     f'counter log prefix "{prefix}outside " group 77 accept']
        else:
            rows += [f'counter log prefix "{prefix}drop " group 77 drop']
        rows += ['}']
    (d / 'gate.nft').write_text('\n'.join([*rows, '}']) + '\n')
    run(['nft', '--check', '-f', d / 'gate.nft'])
    run(['nft', '-f', d / 'gate.nft'])
    run(['sysctl', '-w', 'net.ipv4.ip_forward=1'])
    after = ipv4_settings()
    settings = json.loads((d / 'host-settings.json').read_text())
    settings['task_changed_sysctls'] = {k: v for k, v in before.items() if k in after and after[k] != v}
    save(d / 'host-settings.json', settings)
    save(d / 'network-ready.json', dict(time=now(), lanes=LANES, table=TABLE))


def services(d, runtime):
    for l in LANES:
        lane = d / l['name']
        prefix = d.name + '-' + l['name']
        dns = ['dnsmasq', '--keep-in-foreground', '--conf-file=/dev/null', '--user=builder', '--group=builder',
               '--no-hosts', '--no-resolv', '--server=10.4.10.53', '--listen-address=' + l['host'],
               '--bind-interfaces', '--except-interface=lo', '--log-queries=extra', '--log-facility=-',
               '--cache-size=0', '--pid-file=', '--min-port=1024', '--max-port=60999']
        unit(prefix + '-dns', ['python3', SCRIPTS / 'common.py', 'stream', lane / 'dnsmasq.log', *dns], ROOT, user='root', runtime=runtime)
        unit(prefix + '-pcap', ['tcpdump', '--immediate-mode', '-U', '-n', '-s', '0', '-i', l['veth'],
             '-G', '300', '-w', lane / 'captures/all-%Y%m%dT%H%M%S.pcap'], ROOT, user='root',
             properties={'KillSignal': 'SIGINT', 'StandardOutput': 'append:' + str(lane / 'tcpdump.log'),
                         'StandardError': 'append:' + str(lane / 'tcpdump.log')}, runtime=runtime)
        if l['mode'] == 'observe':
            conf = lane / 'home/.mitmproxy'
            conf.mkdir(mode=0o700)
            run(['chown', 'builder:builder', conf])
            proxy = ['mitmdump', '--mode', 'transparent', '--listen-host', l['host'], '--listen-port', str(l['port']),
                     '--set', 'confdir=' + str(conf), '--set', 'block_global=false', '--set', 'flow_detail=0',
                     '--set', 'save_stream_file=' + str(lane / 'flows-%Y%m%dT%H%M.mitm'), '-s', SCRIPTS / 'proxy.py']
            unit(prefix + '-proxy', ['python3', SCRIPTS / 'common.py', 'stream', lane / 'proxy.log', *proxy], ROOT,
                 env={'HOME': str(lane / 'home'), 'SOAK_LANE': str(lane), 'PYTHONPATH': str(SCRIPTS)}, runtime=runtime)
    for role, interface, target, flt in [
        ('nflog', 'nflog:77', d / 'nflog-%Y%m%dT%H%M%S.pcap', []),
        ('uplink', 'eno1', d / 'uplink-%Y%m%dT%H%M%S.pcap',
         [f'ip and src host {json.loads((d / "nat-source.json").read_text())["ip"]} and (tcp or udp) and src portrange 62000-62999'])]:
        unit(d.name + '-' + role, ['tcpdump', '--immediate-mode', '-U', '-n', '-s', '0', '-i', interface,
             '-G', '300', '-w', target, *flt], ROOT, user='root',
             properties={'KillSignal': 'SIGINT', 'StandardOutput': 'append:' + str(d / (role + '.log')),
                         'StandardError': 'append:' + str(d / (role + '.log'))}, runtime=runtime)
    time.sleep(3)
    for l in LANES:
        if l['mode'] != 'observe':
            continue
        lane = d / l['name']
        ca = lane / 'home/.mitmproxy/mitmproxy-ca-cert.pem'
        for _ in range(30):
            if ca.exists():
                break
            time.sleep(1)
        assert ca.exists(), 'proxy CA missing'
        db = lane / 'home/.pki/nssdb'
        db.mkdir(parents=True)
        run(['chown', '-R', 'builder:builder', lane / 'home'])
        run(['runuser', '-u', 'builder', '--', 'certutil', '-N', '-d', 'sql:' + str(db), '--empty-password'])
        run(['runuser', '-u', 'builder', '--', 'certutil', '-A', '-d', 'sql:' + str(db), '-n', 'alloy-harness', '-t', 'C,,', '-i', ca])


def seal_probe(d):
    probes = []
    for l in LANES:
        prefix = ['ip', 'netns', 'exec', l['ns']]
        tls = prefix + ['curl', '-sS', '-m', '15', '--noproxy', '*']
        if l['mode'] == 'observe':
            tls += ['--cacert', d / l['name'] / 'home/.mitmproxy/mitmproxy-ca-cert.pem']
        result = run([*tls, 'https://example.com'], timeout=18, check=False)
        assert result.returncode != 0 if l['mode'] == 'enforce' else result.returncode == 0 and 'Example Domain' in result.stdout
        udp = run(prefix + ['python3', '-c', "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(2); s.sendto(b'alloy-quic-control',('1.1.1.1',443));\ntry: print(s.recv(1024))\nexcept TimeoutError: print('TIMEOUT_EXPECTED')"], timeout=5)
        assert 'TIMEOUT_EXPECTED' in udp.stdout
        bypass = run(prefix + ['curl', '-sS', '-m', '3', 'http://10.4.10.213:22'], timeout=5, check=False)
        assert bypass.returncode != 0
        kiwix = run(prefix + ['curl', '-sS', '-m', '10', 'http://nomad.home:8090/catalog/v2/entries?count=100'], timeout=13)
        assert '<entry>' in kiwix.stdout
        # The harness control plane must finish its own non-browser egress
        # before the measured browser boundary. The lane consumes this fixture.
        (d / l['name'] / 'prepared-catalog.xml').write_text(kiwix.stdout)
        run(['chown','builder:builder',d/l['name']/'prepared-catalog.xml'])
        probes.append(dict(lane=l['name'], time=now(), tls_rc=result.returncode, tls_body=result.stdout,
                           quic=udp.stdout, host_bypass_rc=bypass.returncode, kiwix=kiwix.stdout))
    save(d / 'seal-probes.json', probes)
    save(d / 'isolation-verified.json', dict(time=now(), browser_runs_so_far=0, lanes=len(LANES)))


def cleanup(d):
    guard()
    assert os.geteuid() == 0
    cleanup_lock = lock(ROOT / 'harness-cleanup.lock')
    ownerpath = ROOT / 'network-owner.json'
    owner = json.loads(ownerpath.read_text()) if ownerpath.exists() else {}
    if owner.get('directory') != str(d):
        if (d / 'restored.json').exists():
            return
        raise RuntimeError('No matching network ownership; refuse cleanup')
    # Return BEFORE touching receipts. A duplicate ExecStopPost cannot mutate a seal.
    if not owner.get('active'):
        return
    producers = [d.name + '-' + l['name'] + '-lane' for l in LANES]
    errors = stop_owned_units(producers, wait=100)
    if errors:
        raise RuntimeError('Producer drain failed; keep providers and strip table: ' + str(errors))
    remaining = {l['ns']: run(['ip', 'netns', 'pids', l['ns']], check=False).stdout.split() for l in LANES}
    if any(remaining.values()):
        raise RuntimeError('Namespace processes remain; keep providers/strip: ' + str(remaining))
    save(d / 'producer-quiesced.json', dict(time=now(), states={name: state(name) for name in producers}))
    time.sleep(3)
    providers = [d.name + '-' + l['name'] + '-' + role for l in LANES
                 for role in ['dns', 'pcap'] + (['proxy'] if l['mode'] == 'observe' else [])]
    providers += [d.name + '-nflog', d.name + '-uplink']
    errors = stop_owned_units(providers, wait=100)
    if errors:
        raise RuntimeError('Provider drain failed: ' + str(errors))
    if (d / 'network-ready.json').exists():
        nft_snapshot(d, 'final-nft.json')
    for l in LANES:
        if pathlib.Path('/run/netns', l['ns']).exists():
            run(['ip', 'netns', 'delete', l['ns']])
        if run(['ip', 'link', 'show', l['veth']], check=False).returncode == 0:
            run(['ip', 'link', 'delete', l['veth']])
        net = pathlib.Path('/etc/netns') / l['ns']
        if net.exists():
            for name in ('resolv.conf', 'nsswitch.conf'):
                (net / name).unlink(missing_ok=True)
            net.rmdir()
    saved = json.loads((d / 'host-settings.json').read_text())
    run(['sysctl', '-w', 'net.ipv4.ip_forward=' + saved['ip_forward']])
    for key, value in saved.get('task_changed_sysctls', {}).items():
        if key != 'net.ipv4.ip_forward':
            run(['sysctl', '-w', key + '=' + value])
    assert all(not pathlib.Path('/run/netns', l['ns']).exists() and
               run(['ip', 'link', 'show', l['veth']], check=False).returncode != 0 for l in LANES)
    if run(['nft', 'list', 'table', 'inet', TABLE], check=False).returncode == 0:
        run(['nft', 'delete', 'table', 'inet', TABLE])
    result = dict(time=now(), ip_forward=run(['sysctl', '-n', 'net.ipv4.ip_forward']).stdout.strip(),
                  namespaces=run(['ip', 'netns', 'list']).stdout, nft=run(['nft', 'list', 'ruleset']).stdout,
                  states={name: state(name) for name in producers + providers})
    assert result['ip_forward'] == saved['ip_forward'] and result['namespaces'] == saved['namespaces'] and result['nft'] == saved['nft'], result
    save(d / 'restored.json', result)
    save(ownerpath, dict(directory=str(d), active=False, time=now(), restored=result))


if __name__ == '__main__':
    assert sys.argv[1] == 'cleanup'
    cleanup(pathlib.Path(sys.argv[2]))
