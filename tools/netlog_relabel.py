#!/usr/bin/env python3
"""Read-only, one-pass bounded-NetLog annotation relabeler (Python stdlib).

Only rg source-path discovery is delegated to a timeout-bounded system command.
No campaign/harness imports. All data writes are beneath --output. Event data
is streamed once; the per-run dedup set retains annotated event keys only.
"""
import argparse
import collections
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
import time
import urllib.parse
import xml.etree.ElementTree as ET

ROOT = pathlib.Path('/home/builder/work')
SOURCE = ROOT / 'brave/src'
CAMPAIGN = ROOT / 'soak/soak02-20261012T094107Z'
KNOWN = CAMPAIGN / 'brave-osr-observe/run-1791884470844417209/netlog-parts-after-stop/event_file_2.json'
CALL = re.compile(r'\b(DefineNetworkTrafficAnnotation|DefinePartialNetworkTrafficAnnotation|CompleteNetworkTrafficAnnotation)\s*\(\s*"([A-Za-z0-9_]+)"', re.S)
FIELDS = ('sender', 'description', 'trigger', 'data', 'destination')
EXPECTED = {
    101845102: ('blink_resource_loader', 541),
    63171670: ('navigation_url_loader', 153),
    107267424: ('open_search', 26),
    112189210: ('favicon_loader', 5),
    54845618: ('update_client', 4),
    130931413: ('service_worker_update_checker', 4),
    115188287: ('chrome_variations_service', 2),
    82509217: ('safe_browsing_v4_update', 1),
    116443055: ('renderer_initiated_download', 1),
}
RULES = [
    {'category': 'unknown', 'rule': 'No matching id, or more than one id for hash (collision).'},
    {'category': 'page', 'rule': 'Exact blink_resource_loader, navigation_url_loader, renderer_initiated_download, service_worker_script_load; service_worker + fetch/update; websocket/web_socket/webtransport/web_transport/webrtc/web_rtc page transport ids excluding explicitly browser/device/service transports.'},
    {'category': 'browser on behalf of page', 'rule': 'Exact favicon_loader, open_search; ids containing prediction, predictor, preconnect, or preresolve.'},
    {'category': 'browser service', 'rule': 'All other known ids. Proposals only, not an allowlist decision.'},
]


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def step(label, started):
    print(f'{utc()} {label}; elapsed={time.monotonic() - started:.3f}s', flush=True)


def annotation_hash(name):
    h = ord(name[0])
    for c in name[1:]:
        h = (h * 31 + ord(c)) % 138003713
    return h


def category(name, ids):
    if len(ids) != 1:
        return 'unknown'
    if name in ('blink_resource_loader', 'navigation_url_loader', 'renderer_initiated_download', 'service_worker_script_load'):
        return 'page'
    if 'service_worker' in name and ('fetch' in name or 'update' in name):
        return 'page'
    transports = ('websocket', 'web_socket', 'webtransport', 'web_transport', 'webrtc', 'web_rtc')
    excluded = ('android', 'cable', 'cast', 'devtools', 'extension', 'signaling', 'signalling', 'service', 'remoting', 'sharing')
    if any(x in name for x in transports) and not any(x in name for x in excluded):
        return 'page'
    if name in ('favicon_loader', 'open_search') or any(x in name for x in ('prediction', 'predictor', 'preconnect', 'preresolve')):
        return 'browser on behalf of page'
    return 'browser service'


def semantics(block):
    """Extract balanced semantics block, respecting quoted text/braces."""
    m = re.search(r'\bsemantics\s*\{', block)
    if not m:
        return None
    start, pos, depth, quoted, escaped = m.end(), m.end(), 1, False, False
    while pos < len(block) and depth:
        c = block[pos]
        if quoted:
            if escaped:
                escaped = False
            elif c == '\\':
                escaped = True
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        pos += 1
    if depth:
        raise ValueError('Unterminated semantics block')
    raw = block[start:pos - 1].strip()
    values = {}
    for field in FIELDS:
        fm = re.search(r'\b' + field + r'\s*:\s*((?:"(?:\\.|[^"\\])*"\s*)+|[A-Z_]+)', raw)
        if fm:
            token = fm.group(1).strip()
            pieces = re.findall(r'"((?:\\.|[^"\\])*)"', token)
            # Source annotation text is not JSON: Brave includes multiline
            # quoted fields. Preserve source escapes/newlines verbatim.
            values[field] = ''.join(pieces) if pieces else token
    return {'text': raw, 'fields': values}


def build_catalog(src):
    started = time.monotonic()
    records = collections.defaultdict(lambda: {'xml_files': [], 'definitions': []})
    items = ET.parse(src / 'tools/traffic_annotation/summary/annotations.xml').getroot().findall('item')
    for item in items:
        records[item.attrib['id']]['xml_files'].append(item.attrib.get('file_path'))
    command = ['timeout', '180', 'rg', '--hidden', '--no-ignore', '-l', '--glob', '!**/.git/**', '--glob', '!out/**',
               r'\b(DefineNetworkTrafficAnnotation|DefinePartialNetworkTrafficAnnotation|CompleteNetworkTrafficAnnotation)\b', '.']
    result = subprocess.run(command, cwd=src, capture_output=True, text=True, timeout=190)
    if result.returncode != 0:
        raise RuntimeError(f'Source discovery failed rc={result.returncode}: {result.stderr}')
    paths = sorted(set(result.stdout.splitlines()))
    for rel in paths:
        path = src / rel
        text = path.read_text(errors='replace')
        calls = list(CALL.finditer(text))
        for index, m in enumerate(calls):
            end = calls[index + 1].start() if index + 1 < len(calls) else len(text)
            # A raw annotation string ends before an unrelated subsequent semantics block.
            block = text[m.end():end]
            raw_end = re.search(r'\)[A-Za-z0-9_]*"', block)
            if raw_end:
                block = block[:raw_end.start()]
            records[m.group(2)]['definitions'].append({
                'file': str(path.relative_to(src)), 'line': text.count('\n', 0, m.start()) + 1,
                'macro': m.group(1), '_source_body': block,
            })
    hashes = collections.defaultdict(list)
    for name in sorted(records):
        hashes[annotation_hash(name)].append(name)
    collisions = {str(h): names for h, names in hashes.items() if len(names) > 1}
    brave = {name for name, data in records.items() if any(d['file'].startswith('brave/') for d in data['definitions'])}
    info = {'xml_item_count': len(items), 'unique_ids': len(records), 'source_files_scanned': len(paths),
            'source_definition_ids': sum(bool(x['definitions']) for x in records.values()),
            'brave_source_ids': len(brave), 'hash_collisions': collisions,
            'scan_command': command, 'scan_elapsed_seconds': time.monotonic() - started}
    if len(items) < 541 or len(records) < 599:
        raise RuntimeError(f'Catalog below plan gate 1: {info}')
    step(f'catalog ids={len(records)} xml={len(items)} brave={len(brave)} collisions={len(collisions)}', started)
    return records, hashes, info


def load_constants(path):
    text = path.read_text()
    # Bounded writer emits {"constants":{...},\n"events": [\n (unclosed header).
    m = re.search(r'"constants"\s*:\s*', text)
    if not m:
        raise ValueError(f'No constants object in {path}')
    constants, _ = json.JSONDecoder().raw_decode(text, m.end())
    return constants


def stream_events(path):
    with path.open() as stream:
        for number, line in enumerate(stream, 1):
            text = line.strip().rstrip(',')
            if not text or text in ('[', ']', ']}', '{', '}'):
                continue
            try:
                event = json.loads(text)
            except json.JSONDecodeError as exc:
                raise ValueError(f'{path}:{number}: {exc}') from exc
            if not isinstance(event, dict) or 'time' not in event or 'source' not in event:
                raise ValueError(f'Unexpected event at {path}:{number}')
            yield event


def blank():
    return {'requests': 0, 'hosts': {}, 'first_time': None, 'last_time': None, 'event_types': {}, 'phases': {}}


def bounds(data, stamp):
    value = float(stamp)
    if data['first_time'] is None or value < float(data['first_time']):
        data['first_time'] = stamp
    if data['last_time'] is None or value > float(data['last_time']):
        data['last_time'] = stamp


def record(data, event, constants):
    params = event['params']
    url = params.get('url', '')
    try:
        host = urllib.parse.urlsplit(url).hostname or '(no host)'
    except ValueError:
        host = '(invalid URL)'
    host_data = data['hosts'].setdefault(host, {'requests': 0, 'sample_urls': []})
    host_data['requests'] += 1
    sample = url[:200]
    if sample not in host_data['sample_urls'] and len(host_data['sample_urls']) < 5:
        host_data['sample_urls'].append(sample)
    data['requests'] += 1
    bounds(data, event['time'])
    types = constants.get('logEventTypes', {})
    typename = next((k for k, v in types.items() if v == event['type']), str(event['type']))
    data['event_types'][typename] = data['event_types'].get(typename, 0) + 1
    phase = str(event.get('phase'))
    data['phases'][phase] = data['phases'].get(phase, 0) + 1


def input_files(campaign, lanes, single):
    if single:
        return sorted({single, single.parent / 'constants.json', single.parent / 'provenance.json'})
    return sorted(p for lane in lanes for p in (campaign / lane).glob('run-*/netlog-parts-*-stop/*')
                  if p.is_file() and (p.name in ('constants.json', 'provenance.json') or re.fullmatch(r'event_file_\d+\.json', p.name)))


def stat_manifest(paths, dest):
    # Exact stat fields required by gate 8, batched below Linux ARG_MAX.
    chunks = []
    for start in range(0, len(paths), 100):
        result = subprocess.run(['stat', '-c', '%n %s %Y', *map(str, paths[start:start + 100])],
                                capture_output=True, text=True, check=True, timeout=20)
        chunks.append(result.stdout)
    text = ''.join(chunks)
    dest.write_text(text)
    return text


def prove_gate(path, hashes, dest):
    started = time.monotonic()
    counts = collections.Counter()
    for event in stream_events(path):
        params = event.get('params', {})
        if 'traffic_annotation' in params:
            counts[int(params['traffic_annotation'])] += 1
    actual = {h: (hashes.get(h, [f'unknown:{h}'])[0], n) for h, n in counts.items()}
    passed = actual == EXPECTED and all(len(hashes[h]) == 1 for h in counts)
    lines = [f'Gate 2: {"PASS" if passed else "FAIL"}', f'Input: {path}',
             'Counting contract: each event whose params contains traffic_annotation; no extra phase/type filter.',
             'hash annotation requests expected']
    for h, (name, n) in sorted(actual.items(), key=lambda x: -x[1][1]):
        lines.append(f'{h} {name} {n} {EXPECTED.get(h)}')
    lines += [f'Total requests: {sum(counts.values())}', f'Elapsed seconds: {time.monotonic() - started:.3f}']
    dest.write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines), flush=True)
    if not passed:
        raise RuntimeError('Gate 2 counts/names differ; stop before lane processing')


def process_run(run, paths, lane_total):
    started = time.monotonic()
    seen = set()
    annotations = {}
    coverage = {'run': run.name, 'first_time': None, 'last_time': None, 'raw_events': 0,
                'annotated_events_raw': 0, 'duplicate_annotated_events': 0, 'requests': 0,
                'after_stop_requests': 0, 'before_only_requests': 0, 'snapshots': [], 'input_event_files': []}
    # after-stop first; retain before-stop-only events, never merge different runs.
    snapshots = sorted({p.parent for p in paths}, key=lambda p: (0 if 'after-stop' in p.name else 1, p.name))
    for snapshot in snapshots:
        constants = load_constants(snapshot / 'constants.json')
        provenance = json.loads((snapshot / 'provenance.json').read_text())
        coverage['snapshots'].append({'path': str(snapshot), 'provenance': provenance,
                                      'timeTickOffset': constants.get('timeTickOffset'),
                                      'logSourceType': constants.get('logSourceType'),
                                      'logEventTypes': constants.get('logEventTypes')})
        for path in sorted((p for p in paths if p.parent == snapshot), key=lambda p: int(re.search(r'\d+', p.stem).group())):
            coverage['input_event_files'].append(str(path))
            for event in stream_events(path):
                coverage['raw_events'] += 1
                bounds(coverage, event['time'])
                params = event.get('params', {})
                if 'traffic_annotation' not in params:
                    continue
                coverage['annotated_events_raw'] += 1
                source = event['source']
                key = (source.get('id'), source.get('start_time'), event['type'], event['time'], event.get('phase'))
                if key in seen:
                    coverage['duplicate_annotated_events'] += 1
                    continue
                seen.add(key)
                h = str(params['traffic_annotation'])
                record(annotations.setdefault(h, blank()), event, constants)
                record(lane_total.setdefault(h, blank()), event, constants)
                coverage['requests'] += 1
                coverage['after_stop_requests' if 'after-stop' in snapshot.name else 'before_only_requests'] += 1
    offsets = {x['timeTickOffset'] for x in coverage['snapshots']}
    if len(offsets) == 1 and None not in offsets and coverage['first_time'] is not None:
        offset = next(iter(offsets))
        for end in ('first', 'last'):
            coverage[end + '_utc'] = datetime.datetime.fromtimestamp((float(coverage[end + '_time']) + float(offset)) / 1000,
                                                                     datetime.timezone.utc).isoformat()
    coverage['elapsed_seconds'] = time.monotonic() - started
    step(f'run {run.name} requests={coverage["requests"]} duplicates={coverage["duplicate_annotated_events"]}', started)
    return {'annotations': annotations, 'coverage': coverage}


def cell(value):
    return str(value).replace('|', '\\|').replace('\n', ' ')


def table(headers, rows):
    return ['| # | ' + ' | '.join(headers) + ' |', '|---|' + '|'.join('---' for _ in headers) + '|'] + [
        '| ' + str(i) + ' | ' + ' | '.join(cell(v) for v in row) + ' |' for i, row in enumerate(rows, 1)]


def write_report(report, dest):
    lines = ['# Surviving NetLog request annotations', '',
             'Categories are proposals. Counts describe surviving annotated events, not the entire soak. '
             'Before/after snapshots are deduplicated within each run; after-stop wins. '
             'Raw event times are NetLog millisecond ticks; UTC coverage uses constants.timeTickOffset. '
             'Coverage includes all surviving events, not just annotated ones. URL samples retain queries, truncated to 200 characters; at most five distinct samples per host per run/annotation and lane/annotation.', '',
             '## Catalog and proposed category rules', '',
             f'Catalog: {report["catalog"]["unique_ids"]} ids; XML {report["catalog"]["xml_item_count"]}; Brave source {report["catalog"]["brave_source_ids"]}.', '',
             'Hash collisions: ' + json.dumps(report['catalog']['hash_collisions']), '']
    lines += table(['Category', 'Proposed rule'], [(r['category'], r['rule']) for r in RULES])
    for lane, data in report['lanes'].items():
        lines += ['', f'## {lane}', '', f'Lane elapsed: {data["elapsed_seconds"]:.3f}s', '', '### Per annotation', '']
        rows = []
        for h, d in sorted(data['annotations'].items(), key=lambda x: -x[1]['requests']):
            meta = report['seen_annotations'][h]
            rows.append((meta['label'], h, meta['category'], d['requests'], len(d['hosts']), ', '.join(sorted(d['hosts'])),
                         '<br>'.join(u for hd in d['hosts'].values() for u in hd['sample_urls']), d['first_time'], d['last_time']))
        lines += table(['Annotation', 'Hash', 'Category', 'Requests', 'Distinct hosts', 'Hosts', 'Sample URLs', 'First tick', 'Last tick'], rows)
        lines += ['', '### Per-run surviving coverage', '']
        lines += table(['Run', 'Event files', 'First tick', 'Last tick', 'First UTC', 'Last UTC', 'Raw events', 'Requests', 'Duplicates', 'Before-only'], [
            (r, len(d['coverage']['input_event_files']), d['coverage']['first_time'], d['coverage']['last_time'],
             d['coverage'].get('first_utc'), d['coverage'].get('last_utc'), d['coverage']['raw_events'], d['coverage']['requests'],
             d['coverage']['duplicate_annotated_events'], d['coverage']['before_only_requests']) for r, d in data['runs'].items()])
        for run, rd in data['runs'].items():
            lines += ['', f'### {run}: per annotation', '']
            lines += table(['Annotation', 'Category', 'Requests', 'Distinct hosts', 'Hosts', 'Sample URLs', 'First tick', 'Last tick'], [
                (report['seen_annotations'][h]['label'], report['seen_annotations'][h]['category'], d['requests'], len(d['hosts']),
                 ', '.join(sorted(d['hosts'])), '<br>'.join(u for hd in d['hosts'].values() for u in hd['sample_urls']),
                 d['first_time'], d['last_time']) for h, d in sorted(rd['annotations'].items(), key=lambda x: -x[1]['requests'])])
    lines += ['', '## Seen annotation definitions and semantics', '']
    for h, meta in report['seen_annotations'].items():
        lines += [f'### {meta["label"]} ({h})', '']
        if not meta['ids']:
            lines += ['Unknown hash; all hosts/counts retained.', '']
        for name in meta['ids']:
            definitions = report['definitions'][name]
            lines += [f'Id: `{name}`. XML definition paths: {definitions["xml_files"]}', '']
            for d in definitions['definitions']:
                lines += [f'`{d["file"]}:{d["line"]}` ({d["macro"]})', '', '```text',
                          d['semantics']['text'] if d['semantics'] else '(No semantics block in this definition; partial/completion or source scan limitation.)', '```', '']
    lines += ['## Integrity and timing', '', f'Input stat manifest unchanged: {report["inputs_stat_unchanged"]}.',
              f'Total elapsed seconds: {report["elapsed_seconds"]:.3f}.', '']
    (dest / 'relabel.md').write_text('\n'.join(lines))
    (dest / 'relabel.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=pathlib.Path, default=SOURCE)
    parser.add_argument('--campaign', type=pathlib.Path, default=CAMPAIGN)
    parser.add_argument('--lane', action='append', default=[])
    parser.add_argument('--file', type=pathlib.Path)
    parser.add_argument('--gate2', action='store_true', help='Prove known file before processing any lane')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if bool(args.file) == bool(args.lane):
        parser.error('Choose --file or one/more --lane (no implicit all-lanes run)')
    started = time.monotonic()
    dest = args.output.resolve()
    if dest.exists():
        parser.error('Output must be a new directory; preserve earlier evidence')
    if dest.is_relative_to(args.campaign.resolve()) or dest.is_relative_to(args.source.resolve()):
        parser.error('Output cannot be inside read-only campaign or source')
    dest.mkdir(parents=True)
    lanes = args.lane or [args.file.parent.parent.parent.name]
    paths = input_files(args.campaign, lanes, args.file)
    if not paths:
        raise RuntimeError('No inputs matched')
    before = stat_manifest(paths, dest / 'inputs-stat-before.txt')
    records, hashes, catalog = build_catalog(args.source)
    if args.gate2:
        prove_gate(KNOWN, hashes, dest / 'gate2.txt')
    report = {'created_utc': utc(), 'source': str(args.source), 'campaign': str(args.campaign),
              'script_sha256': hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
              'catalog': catalog, 'category_rules': RULES, 'lanes': {}, 'seen_annotations': {}, 'definitions': {}}
    for lane in lanes:
        lane_started = time.monotonic()
        event_paths = [p for p in paths if re.fullmatch(r'event_file_\d+\.json', p.name) and (args.file or p.parent.parent.parent.name == lane)]
        runs = sorted({p.parent.parent for p in event_paths})
        totals, run_results = {}, {}
        for run in runs:
            run_results[run.name] = process_run(run, [p for p in event_paths if p.parent.parent == run], totals)
        report['lanes'][lane] = {'annotations': totals, 'runs': run_results, 'elapsed_seconds': time.monotonic() - lane_started}
        for h in totals:
            ids = hashes.get(int(h), [])
            label = ids[0] if len(ids) == 1 else ('collision:' + h + ':' + ','.join(ids) if ids else 'unknown:' + h)
            report['seen_annotations'][h] = {'ids': ids, 'label': label, 'category': category(label, ids)}
            for name in ids:
                if name not in report['definitions']:
                    data = records[name]
                    report['definitions'][name] = {
                        'xml_files': data['xml_files'],
                        'definitions': [dict(file=d['file'], line=d['line'], macro=d['macro'],
                                             semantics=semantics(d['_source_body'])) for d in data['definitions']],
                    }
        step(f'lane={lane} annotations={len(totals)} unknown_hashes={sum(not hashes.get(int(h)) for h in totals)}', lane_started)
    after = stat_manifest(paths, dest / 'inputs-stat-after.txt')
    report['inputs_stat_unchanged'] = before == after
    report['elapsed_seconds'] = time.monotonic() - started
    write_report(report, dest)
    if before != after:
        raise RuntimeError('Input stat manifest changed')
    step(f'RESULT {dest}', started)


if __name__ == '__main__':
    main()
