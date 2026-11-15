#!/usr/bin/env python3
"""Correlate one boot's Venus kernel and decoder logs. Observations cannot establish playback acceptance.

Pass a kernel ring and decoder logs already restricted to the same boot. A merged historical logarchive is not a boot selector.
Only metadata is emitted: the parser neither reads pixels nor connects to a device.
"""
import argparse
import json
from pathlib import Path
import re

KEY = re.compile(r'session (0x[0-9a-fA-F]+) gen (\d+) token (\d+)')
PAIRS = r'([0-9a-fA-F]{16}) UV ([0-9a-fA-F]{16}) zero-bytes (\d+)/(\d+) zero-rows (\d+)/(\d+)'
STAGES = ('copied-locked', 'after-logging-locked', 'after-map-query-locked', 'pre-emit-relocked')
PLANES = ('source-Y', 'source-UV', 'target-Y', 'target-UV')


def analyse(kernel, decoder):
    frames, completions = {}, {}
    malformed = []
    performance = {'clocks': [], 'cumulative': [], 'coverage': [], 'cpu': [], 'scheduling': [], 'slow_frames': [], 'slow_inputs': []}

    def numbers(match):
        return [int(x) for x in match.groups()]

    def put(frame, kind, value):
        previous = frame['records'].get(kind)
        if previous is not None and previous != value:
            frame['conflicts'].append(kind)
        else:
            frame['records'][kind] = value

    for source, text in (('kernel', kernel), ('decoder', decoder)):
        for line_number, line in enumerate(text.splitlines(), 1):
            if 'COACHZ venus:' not in line and 'AVD coachz:' not in line:
                continue
            # Actual lightweight performance records are independent of selected-picture integrity observations. Keep raw ticks
            # and coverage, including malformed records; never turn live cumulative counters into a hardware-only decode time.
            perf_kind = next((kind for marker, kind in (
                ('AVD coachz: perf clock ', 'clocks'), ('AVD coachz: perf coverage ', 'coverage'),
                ('AVD coachz: perf cpu ', 'cpu'), ('AVD coachz: perf scheduling ', 'scheduling'),
                ('AVD coachz: perf session ', 'cumulative'), ('AVD coachz: slow frame ', 'slow_frames'),
                ('AVD coachz: slow input ', 'slow_inputs')) if marker in line), None)
            if perf_kind:
                perf_session = re.search(r'session (0x[0-9a-fA-F]+)', line)
                if not perf_session:
                    malformed.append({'source': source, 'line': line_number, 'reason': 'missing performance session'})
                    continue
                item = {'session': perf_session[1], 'line': line_number, 'source': source}
                body = line[perf_session.end():]
                for name, value in re.findall(r'([a-z][a-z0-9-]*) (\d+)(?=\s|$|\.\.)', body):
                    item[name] = int(value)
                if perf_kind == 'scheduling':
                    status = re.search(r' status (0x[0-9a-f]+)', body)
                    if status:
                        item['status'] = int(status[1], 16)
                    for name, value in re.findall(r'(qos-status|relative|policy|priority|current) (-?\d+)', body):
                        item[name] = int(value)
                stage = re.search(r' stage (\S+)', body)
                if stage:
                    item['stage'] = stage[1]
                interval = re.search(r' ticks (\d+)\.\.(\d+)', body)
                if interval:
                    item['interval_ticks'] = [int(interval[1]), int(interval[2])]
                required = {'clocks': ('version', 'ns-numer', 'ns-denom', 'slow-ticks'),
                    'coverage': ('frames', 'missing-fbd', 'missing-ebd', 'missing-etb', 'invalid'),
                    'cpu': ('frames', 'samples', 'nanoseconds', 'failed'),
                    'scheduling': ('status', 'qos-status', 'qos', 'relative', 'policy', 'priority', 'current', 'event-worker'),
                    'cumulative': ('frames', 'stage', 'n', 'ticks', 'max', 'cumulative-live'),
                    'slow_frames': ('gen', 'token', 'n', 'interval_ticks', 'etb', 'message', 'batch', 'pool', 'lock', 'snapshot',
                                    'copy', 'audit', 'unlock', 'attachments', 'return', 'pending', 'pre-emit', 'emit', 'emits', 'sampled', 'cpu-ns', 'cpu-valid'),
                    'slow_inputs': ('token', 'n', 'interval_ticks', 'rate', 'block', 'input-wait', 'annex-b', 'hash', 'queue', 'bytes')}
                missing = [name for name in required[perf_kind] if name not in item]
                if missing:
                    item['gaps'] = missing
                    malformed.append({'source': source, 'line': line_number, 'reason': 'incomplete performance record', 'missing': missing})
                performance[perf_kind].append(item)
                continue
            completion = re.search(r'input completion session (0x[0-9a-fA-F]+) token (\d+) bytes (\d+) before ([0-9a-f]{16}) after ([0-9a-f]{16}) error (0x[0-9a-f]+)', line)
            if completion:
                session, token, size, before, after, error = completion.groups()
                key = (int(session, 16), int(token))
                value = {'bytes': int(size), 'before': before, 'after': after, 'error': int(error, 16)}
                if key in completions and completions[key] != value:
                    malformed.append({'source': source, 'line': line_number, 'reason': 'conflicting input completion'})
                completions[key] = value
                continue
            key = KEY.search(line)
            if not key:
                if any(marker in line for marker in ('full FBD session', 'full snapshot session', 'audit session',
                        'mapping session', 'input reference session', 'input completion session', 'frame metadata session')):
                    malformed.append({'source': source, 'line': line_number, 'reason': 'incomplete frame identity'})
                continue
            session, generation, token = int(key[1], 16), int(key[2]), int(key[3])
            frame = frames.setdefault((session, generation, token), {'session': hex(session), 'generation': generation,
                'token': token, 'sources': [], 'records': {}, 'conflicts': []})
            if source not in frame['sources']:
                frame['sources'].append(source)
            records = frame['records']
            body = line[key.end():]
            index = re.match(r' index (\d+)', body)
            if index:
                put(frame, 'index', int(index[1]))
            known = False
            full = re.search(r'Y ' + PAIRS, body)
            if full and ('full FBD session' in line or 'full snapshot session' in line):
                kind = 'kernel-full' if 'full FBD session' in line else 'snapshot'
                value = {'Y': full[1], 'UV': full[2], 'zero_bytes': [int(full[3]), int(full[4])],
                         'zero_rows': [int(full[5]), int(full[6])]}
                interval = re.search(r'ticks (\d+)\.\.(\d+)', body)
                if interval:
                    value['observation_ticks'] = numbers(interval)
                put(frame, kind, value)
                known = True
            if 'audit session' in line:
                stage = re.search(r'stage (\S+)', body)
                destination = re.search(r'dst-diff (\d+)/(\d+)', body)
                source_diff = re.search(r'src-diff (\d+)/(\d+)', body)
                if stage and destination:
                    value = {'destination_diff': numbers(destination)}
                    if source_diff:
                        value['source_diff'] = numbers(source_diff)
                    rows = re.search(r'(?:dst-rows| rows) (\d+)/(\d+)', body)
                    first = re.search(r'first (\d+):(\d+)/(\d+):(\d+)', body)
                    zeros = re.search(r'(?:dst-zero| zero) (\d+)/(\d+)', body)
                    if rows and first and zeros:
                        value.update(destination_rows=numbers(rows), first_difference=numbers(first),
                                     zero_difference_bytes=numbers(zeros))
                    interval = re.search(r'ticks (\d+)\.\.(\d+) capture (\d+)\.\.(\d+) copied (\d+)', body)
                    tick = re.search(r'ticks (\d+)', body)
                    if interval:
                        times = numbers(interval)
                        value.update(observation_ticks=times[:2], capture_ticks=times[2:4], copied_ticks=times[4])
                    elif stage[1] == 'pre-emit-relocked' and tick:
                        value['observation_ticks'] = [int(tick[1]), int(tick[1])]
                    returned = re.search(r'return (0x[0-9a-f]+) unlock (-?\d+)', body)
                    if returned:
                        value.update(return_status=int(returned[1], 16), unlock_status=int(returned[2]))
                    put(frame, 'audit:' + stage[1], value)
                    known = True
            if 'mapping session' in line:
                mapping = re.search(r'plane (\S+) status (0x[0-9a-f]+) result-bytes (\d+) version (\d+) span (\d+) pages (\d+) unmapped (\d+) unmanaged (\d+) changed (\d+) attr-mask (0x[0-9a-f]+)', body)
                if mapping:
                    plane, status, size, version, span, pages, unmapped, unmanaged, changed, attrs = mapping.groups()
                    put(frame, 'mapping:' + plane, {'status': int(status, 16), 'result_bytes': int(size), 'version': int(version),
                        'span': int(span), 'pages': int(pages), 'unmapped': int(unmapped), 'unmanaged': int(unmanaged),
                        'changed': int(changed), 'attribute_mask': int(attrs, 16)})
                    known = True
            if 'input reference session' in line:
                reference = re.search(r'found (\d+) bytes (\d+) before ([0-9a-f]{16}) completed (\d+) after ([0-9a-f]{16}) error (0x[0-9a-f]+)', body)
                if reference:
                    found, size, before, completed, after, error = reference.groups()
                    put(frame, 'input', {'found': bool(int(found)), 'bytes': int(size), 'before': before,
                        'completed': bool(int(completed)), 'after': after, 'error': int(error, 16)})
                    known = True
            if 'frame metadata session' in line:
                metadata = re.search(r'matched (\d+) pts (-?\d+)/(\d+) flags (0x[0-9a-f]+) duration (-?\d+)/(\d+) flags (0x[0-9a-f]+) queued-bytes (\d+) input-hash ([0-9a-f]{16})', body)
                if metadata:
                    matched, pts, scale, flags, duration, duration_scale, duration_flags, size, digest = metadata.groups()
                    put(frame, 'metadata', {'matched': bool(int(matched)), 'pts': [int(pts), int(scale), int(flags, 16)],
                        'duration': [int(duration), int(duration_scale), int(duration_flags, 16)], 'bytes': int(size), 'input_hash': digest})
                    known = True
            simple = (
                ('return session', 'return', r'fbd (\d+) returned (\d+) submitted (\d+) requeue (\d+) ok (\d+)'),
                ('dequeue session', 'dequeue', r'ticks (\d+) queued (\d+) lost (\d+)'),
                ('unlock session', 'unlock', r'ticks (\d+) status (-?\d+)'),
                ('emit session', 'emit', r'ticks (\d+)\.\.(\d+) result (-?\d+) frame-status (-?\d+)'),
            )
            for marker, kind, pattern in simple:
                if marker in line:
                    match = re.search(pattern, body)
                    if match:
                        put(frame, kind, numbers(match))
                        known = True
            if 'copy session' in line:
                diff = re.search(r'diff Y (\d+) bytes/(\d+) rows first (\d+):(\d+) zero (\d+) UV (\d+) bytes/(\d+) rows first (\d+):(\d+) zero (\d+)', body)
                if diff:
                    values = numbers(diff)
                    put(frame, 'legacy-copy', {'difference_bytes': [values[0], values[5]], 'zero_difference_bytes': [values[4], values[9]]})
                    known = True
            if 'sample session' in line or 'audit layout session' in line:
                known = True  # Sparse hashes and layout alone are never a full comparison.
            if not known:
                malformed.append({'source': source, 'line': line_number, 'reason': 'unparsed or incomplete frame record'})

    for frame in frames.values():
        records = frame['records']
        gaps, observed = [], []
        required = ['kernel-full', 'snapshot', 'input', 'metadata', 'dequeue', 'unlock', 'return', 'emit']
        required += ['audit:' + stage for stage in STAGES] + ['mapping:' + plane for plane in PLANES]
        gaps.extend(kind for kind in required if kind not in records)
        if frame['conflicts']:
            gaps.append('conflicting records; check boot selection or token reuse')
        for stage in STAGES:
            audit = records.get('audit:' + stage)
            if audit:
                if any(audit.get('source_diff', [])):
                    observed.append('snapshot/source difference at ' + stage)
                if any(audit['destination_diff']):
                    observed.append('snapshot/destination difference at ' + stage)
                if audit.get('return_status', 0) or audit.get('unlock_status', 0):
                    observed.append('pre-emit return/unlock error')
        if 'kernel-full' in records and 'snapshot' in records:
            if any(records['kernel-full'][plane] != records['snapshot'][plane] for plane in ('Y', 'UV')):
                observed.insert(0, 'FBD/snapshot digest difference')
        if any(records.get('legacy-copy', {}).get('difference_bytes', [])):
            observed.append('legacy full-plane copy difference; no retained pre-copy reference')
        for plane in PLANES:
            mapping = records.get('mapping:' + plane)
            if mapping:
                if mapping['status'] or mapping['version'] != 1 or mapping['result_bytes'] != 24:
                    gaps.append('mapping unavailable: ' + plane)
                elif not mapping['pages'] or not mapping['span']:
                    gaps.append('invalid mapping extent: ' + plane)
                elif mapping['unmapped'] or mapping['unmanaged'] or mapping['changed']:
                    gaps.append('mapping incomplete or changed: ' + plane)
        reference, metadata = records.get('input'), records.get('metadata')
        if reference:
            if not reference['found']:
                gaps.append('input history association missing')
            else:
                late = completions.get((int(frame['session'], 16), frame['token']))
                if not reference['completed'] and late:
                    if late['before'] != reference['before'] or late['bytes'] != reference['bytes']:
                        gaps.append('late input completion association conflicts')
                    else:
                        reference.update(completed=True, after=late['after'], error=late['error'])
                if not reference['completed']:
                    gaps.append('input completion not captured')
                elif reference['before'] != reference['after']:
                    observed.append('compressed input changed while firmware owned it')
                if reference['error']:
                    observed.append('firmware input error')
                if metadata and metadata['matched']:
                    if metadata['input_hash'] != reference['before'] or metadata['bytes'] != reference['bytes']:
                        observed.append('decoder/kernel compressed input difference')
        if metadata and not metadata['matched']:
            gaps.append('pending frame metadata missing')
        if records.get('dequeue', [0, 0, 0])[2]:
            gaps.append('client events lost')
        returned, emitted = records.get('return'), records.get('emit')
        if returned and not returned[4]:
            observed.append('output requeue error')
        if records.get('unlock', [0, 0])[1]:
            observed.append('pixel buffer unlock error')
        if emitted and (emitted[2] or emitted[3]):
            observed.append('VideoToolbox emit or frame error')
        if returned and emitted and not (returned[0] <= returned[1] <= returned[2] <= emitted[0] <= emitted[1]):
            gaps.append('inconsistent return/emission timeline')
        timeline = records.get('kernel-full', {}).get('observation_ticks', [])[:]
        if 'kernel-full' in records and not timeline:
            gaps.append('missing FBD observation timeline')
        if 'dequeue' in records:
            timeline.append(records['dequeue'][0])
        captured = None
        for stage in STAGES:
            audit = records.get('audit:' + stage)
            if not audit:
                continue
            if 'destination_rows' not in audit:
                gaps.append('incomplete comparison counters: ' + stage)
            else:
                for size, rows, zeros in zip(audit['destination_diff'], audit['destination_rows'], audit['zero_difference_bytes']):
                    if (size == 0) != (rows == 0) or zeros > size:
                        gaps.append('inconsistent comparison counters: ' + stage)
                        break
            if 'observation_ticks' not in audit:
                gaps.append('missing observation timeline: ' + stage)
            if stage == STAGES[0] and 'capture_ticks' in audit:
                captured = audit['capture_ticks'] + [audit['copied_ticks']]
                timeline.extend(captured)
            if 'capture_ticks' in audit and captured != audit['capture_ticks'] + [audit['copied_ticks']]:
                gaps.append('conflicting snapshot timeline: ' + stage)
            if stage != STAGES[-1]:
                timeline.extend(audit.get('observation_ticks', []))
        if 'unlock' in records:
            timeline.append(records['unlock'][0])
        if returned:
            timeline.extend(returned[1:3])
        timeline.extend(records.get('audit:' + STAGES[-1], {}).get('observation_ticks', []))
        if emitted:
            timeline.extend(emitted[:2])
        if timeline != sorted(timeline):
            gaps.append('inconsistent observed frame timeline')
        frame.update(gaps=gaps, observed=observed,
            coverage='incomplete' if gaps else 'all requested records present',
            interpretation='differences/errors observed' if observed else 'no difference in captured observations; playback unverified')
    ordered = sorted(frames.values(), key=lambda item: (item['session'], item['generation'], item['token']))
    return {'scope': 'one boot; selected pictures only; no playback acceptance', 'frames': ordered, 'unparsed': malformed,
        'performance': performance,
        'summary': {'pictures': len(ordered), 'pictures_with_gaps': sum(bool(f['gaps']) for f in ordered),
                    'pictures_with_decoder_evidence': sum('decoder' in f['sources'] for f in ordered),
                    'pictures_with_observed_differences_or_errors': sum(bool(f['observed']) for f in ordered)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True, help='one boot kernel log ring')
    parser.add_argument('--decoder', type=Path, required=True, help='decoder log already restricted to that same boot')
    parser.add_argument('--output', type=Path, help='write metadata JSON; default stdout')
    args = parser.parse_args()
    report = analyse(args.kernel.read_text(errors='replace'), args.decoder.read_text(errors='replace'))
    result = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(result)
    else:
        print(result, end='')


if __name__ == '__main__':
    main()
