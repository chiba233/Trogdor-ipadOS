#!/usr/bin/env python3
"""Read bounded GPU profile v3 blocks and completed Mesa CSV batches.

Host uptime and GPU timestamps remain separate. Report span durations and
explicit missing data; nested spans must never be added as independent costs.
"""
# SPDX-License-Identifier: BSD-2-Clause
import argparse
import bisect
import collections
import csv
import json
from pathlib import Path
import re
import struct

BLOCK = struct.Struct('<QQQIIIIQQ')
END = struct.Struct('<QQ')
RECORD = struct.Struct('<II4Q8I')
MAGIC, END_MAGIC = 0x33505A4843414F43, 0x3350555047444E45
FIELDS = 'type idx t0 t1 t2 t3 a b c d tid cpu0 cpu1 tid2'.split()


def checksum(data, seed=14695981039346656037):
    for byte in data:
        seed = ((seed ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return seed


def read_frames(data):
    """Accept only complete, authenticated, ordered blocks before the first error."""
    records, offset, expected, dropped, identity = [], 0, 0, 0, None
    reason = None if data else 'empty file'
    while offset < len(data):
        if len(data) - offset < BLOCK.size:
            reason = 'partial header'
            break
        header = list(BLOCK.unpack_from(data, offset))
        magic, epoch, first, version, size, count, pid, lost, digest = header
        if magic != MAGIC or version != 3 or size != RECORD.size or not 1 <= count <= 256:
            reason = 'invalid header (v3 required)'
            break
        total = BLOCK.size + count * size + END.size
        if len(data) - offset < total:
            reason = 'partial payload/footer'
            break
        payload = data[offset + BLOCK.size:offset + total - END.size]
        footer = END.unpack_from(data, offset + total - END.size)
        header[-1] = 0
        calculated = checksum(payload, checksum(BLOCK.pack(*header)))
        if footer != (END_MAGIC, digest) or calculated != digest:
            reason = 'checksum/footer mismatch'
            break
        if identity is not None and identity != (pid, epoch):
            reason = 'process/epoch changed inside file'
            break
        if first < expected or first - expected != lost:
            reason = 'unexplained record sequence gap/overlap'
            break
        identity = pid, epoch
        expected, dropped = first + count, dropped + lost
        records.extend(dict(zip(FIELDS, row)) for row in RECORD.iter_unpack(payload))
        offset += total
    rings = {}
    for name, kind, field in [('runtime', 12, 'c'), ('kernel_submit', 8, 't1'), ('render_pass', 7, 't1')]:
        sequence = [r[field] for r in records if r['type'] == kind]
        gaps = [dict(after=a, before=b) for a, b in zip(sequence, sequence[1:])
                if (b - a) & 0xffffffff != 1]
        rings[name] = dict(first=sequence[0] if sequence else None,
                           last=sequence[-1] if sequence else None, gaps=gaps)
    return records, dict(pid=identity[0] if identity else None,
                         epoch=identity[1] if identity else None,
                         valid_bytes=offset, rejected_bytes=len(data) - offset,
                         records=len(records), dropped_records=dropped, stop=reason,
                         source_rings=rings)


def attributes(cells):
    return {key.strip(): value.strip() for cell in cells if '=' in cell
            for key, value in [cell.split('=', 1)]}


def work_report():
    return dict(invalid_lines=[], incomplete_batches=[], identities=[],
                error_counts={}, completed_batches=0, span_count=0,
                unexecuted_or_unmeasured_ranges=0, peak_pending_events=0)


def work_issue(report, field, reason, **detail):
    counts = report['error_counts']
    counts[reason] = counts.get(reason, 0) + 1
    # Retain examples, not millions of copies of the same missing-data row.
    if len(report[field]) < 100:
        report[field].append(dict(reason=reason, **detail))


def work_spans(lines, report, submissions=None, metadata_only=False):
    """Stream completed CSV batches. Unknown conditional ranges never erase valid siblings.

    A 0/0 pair has no measured execution; neither a zero-duration operation nor
    proof of transport corruption. One missing endpoint remains an error.
    """
    pending, links, completed = {}, collections.defaultdict(set), set()
    pending_events = 0
    for lineno, line in enumerate(lines, 1):
        if not line.endswith('\n'):
            work_issue(report, 'invalid_lines', 'partial line', line=lineno)
            continue
        try:
            row = next(csv.reader([line]))
            if not row or row[0] in ('frame', ''):
                continue
            tag = row[0]
            if tag == 'profile':
                if len(row) != 4 or row[1] != '1':
                    raise ValueError('unknown profile identity')
                identity = dict(pid=int(row[2]), epoch=int(row[3]))
                if identity not in report['identities']:
                    report['identities'].append(identity)
            elif tag == 'submit':
                if len(row) != 8:
                    raise ValueError('submit fields')
                if submissions is not None:
                    values = [int(v, 0) for v in row[1:]]
                    submissions.append(dict(zip(('host_ns', 'submission', 'queue', 'fence',
                                                 'command', 'index', 'queue_address'), values)))
            elif metadata_only:
                continue
            elif tag == 'batch_link':
                if len(row) != 7:
                    raise ValueError('batch link fields')
                frame, batch, submission, queue, fence, ready = map(int, row[1:])
                links[frame, batch].add((submission, queue, fence, ready))
            elif tag == 'batch_end':
                if len(row) != 6:
                    raise ValueError('batch marker fields')
                frame, batch, count, host, error = map(int, row[1:])
                key = frame, batch
                events = pending.pop(key, [])
                pending_events -= len(events)
                batch_links = links.pop(key, set())
                if key in completed:
                    work_issue(report, 'incomplete_batches', 'duplicate completion marker', batch=key)
                    continue
                completed.add(key)
                report['completed_batches'] += 1
                if count != len(events) or error:
                    work_issue(report, 'incomplete_batches', 'missing/error/count-mismatched completion marker', batch=key)
                    continue
                fence_link = None
                if len(batch_links) == 1:
                    submission, queue, fence, ready = next(iter(batch_links))
                    if ready == 0:
                        fence_link = dict(submission=submission, queue=queue, fence=fence)
                    else:
                        work_issue(report, 'incomplete_batches', 'GPU fence wait failed', batch=key)
                else:
                    work_issue(report, 'incomplete_batches', 'missing/ambiguous GPU fence link', batch=key)
                stack, spans, damaged = [], [], False
                for event in events:
                    name = event['event']
                    if name.startswith('start_'):
                        stack.append(event)
                    elif name.startswith('end_'):
                        if not stack or stack[-1]['event'][6:] != name[4:]:
                            work_issue(report, 'incomplete_batches', 'unbalanced ranges', batch=key, line=event['line'])
                            damaged = True
                            break
                        begin = stack.pop()
                        if not begin['ns'] and not event['ns']:
                            report['unexecuted_or_unmeasured_ranges'] += 1
                            continue
                        if not begin['ns'] or not event['ns'] or event['ns'] < begin['ns']:
                            work_issue(report, 'incomplete_batches', 'missing/backwards timestamp', batch=key, line=event['line'])
                            damaged = True
                            continue
                        args = begin['args'] | event['args']
                        command = begin['args'].get('command_buffer_handle')
                        if command is None:
                            command = next((e['args']['command_buffer_handle'] for e in reversed(stack)
                                            if 'command_buffer_handle' in e['args']), None)
                        spans.append(dict(frame=frame, batch=batch, name=name[4:],
                                          begin_ns=begin['ns'], end_ns=event['ns'],
                                          duration_ns=event['ns'] - begin['ns'], depth=len(stack),
                                          command=int(command) if command else None,
                                          args=args, fence_link=fence_link))
                if stack:
                    damaged = True
                    work_issue(report, 'incomplete_batches', 'unterminated ranges', batch=key)
                for span in spans:
                    span['batch_ranges_complete'] = not damaged
                    report['span_count'] += 1
                    yield span
            else:
                frame, batch, ns = map(int, row[:3])
                if (frame, batch) in completed:
                    work_issue(report, 'invalid_lines', 'event after completed batch', batch=(frame, batch), line=lineno)
                    continue
                pending.setdefault((frame, batch), []).append(
                    dict(ns=ns, event=row[3], args=attributes(row[4:]), line=lineno))
                pending_events += 1
                report['peak_pending_events'] = max(report['peak_pending_events'], pending_events)
                if pending_events > 1000000:
                    raise RuntimeError('over one million events without completed batches; refusing unbounded input')
        except (ValueError, IndexError, csv.Error) as exc:
            work_issue(report, 'invalid_lines', str(exc), line=lineno)
    for key in pending.keys() | links.keys():
        work_issue(report, 'incomplete_batches', 'missing completion marker', batch=key)


def read_work(text):
    """Compatibility entry for small in-memory captures; the CLI streams files."""
    report, submissions = work_report(), []
    spans = list(work_spans(text.splitlines(keepends=True), report, submissions))
    return spans, submissions, report


class PipelineJournal:
    """Match actual Vulkan handles within their creation/destruction lifetimes.

    Full executable hashes must agree. Names are all aliases observed before
    submission, never a guess from a canonical Vulkan entry point named main.
    """
    def __init__(self, text):
        self.handles = collections.defaultdict(list)
        self.shaders = collections.defaultdict(list)
        self.legacy_names = pipeline_names(text)
        self.callbacks = []
        self.unresolved_aliases = 0
        self.alias_truncations = 0
        self.rejected_records = 0
        for line in text.splitlines():
            head, _, alias = line.partition(' | ')
            fields = dict(re.findall(r'(\w+)=([^ ]+)', head))
            try:
                if line.startswith('P at='):
                    at, raw, bind = (int(fields[k]) for k in ('at', 'vk', 'bind'))
                    lives = self.handles[raw]
                    if bind == 0xffffffff:
                        if lives:
                            lives[-1]['end_us'] = at
                        continue
                    if lives and lives[-1]['end_us'] is None:
                        lives[-1]['end_us'] = at
                    lives.append(dict(begin_us=at, end_us=None, bind=bind,
                                      hashes={k: fields[k].lower() for k in ('vs', 'fs', 'cs')}, aliases=[]))
                elif line.startswith('A at='):
                    at, raw = int(fields['at']), int(fields['vk'])
                    lengths = re.search(r'\bfull(\d+) kept(\d+)', head)
                    truncated = not lengths or lengths[1] != lengths[2]
                    self.alias_truncations += truncated
                    lives = self.handles.get(raw, [])
                    if lives and lives[-1]['begin_us'] <= at and lives[-1]['end_us'] is None:
                        lives[-1]['aliases'].append(dict(at_us=at, name=alias, truncated=truncated))
                    else:
                        self.unresolved_aliases += 1
                elif line.startswith('S at='):
                    code = fields['code'].lower()
                    if not re.fullmatch(r'[0-9a-f]{64}', code) or not int(code, 16):
                        raise ValueError('empty/truncated executable hash')
                    stat = {k: int(v) for k, v in fields.items() if k != 'code'}
                    self.shaders[stat['stage'], code].append(stat)
                elif line.startswith('CALLBACKS '):
                    self.callbacks.append({k: int(v) for k, v in fields.items()})
            except (ValueError, KeyError):
                self.rejected_records += 1
        self.starts = {key: [life['begin_us'] for life in lives] for key, lives in self.handles.items()}

    def annotate(self, span, host_ns):
        raw = span['args'].get('pipeline_handle')
        span['pipeline_candidates'] = []
        if raw is None or host_ns is None:
            shader = span['args'].get('cs_sha1', '')
            span['pipeline_candidates'] = self.legacy_names.get(shader[:8].lower(), [])
            span['pipeline_identity'] = 'legacy_hash_only' if span['pipeline_candidates'] else 'unresolved'
            return
        at = host_ns // 1000
        lives = self.handles.get(int(raw), [])
        index = bisect.bisect_right(self.starts.get(int(raw), []), at) - 1
        if index < 0 or (lives[index]['end_us'] is not None and at >= lives[index]['end_us']):
            span['pipeline_identity'] = 'no_live_handle'
            return
        life = lives[index]
        mismatches, shader_stats = [], {}
        for stage, mesa_stage in [('vs', 0), ('fs', 4), ('cs', 5)]:
            code = span['args'].get(stage + '_sha1')
            if code and int(code, 16):
                if code.lower() != life['hashes'][stage]:
                    mismatches.append(stage)
                stats = [stat for stat in self.shaders.get((mesa_stage, code.lower()), []) if stat['at'] <= at]
                if stats:
                    shader_stats[stage] = stats[-1]
        aliases = [a for a in life['aliases'] if a['at_us'] <= at]
        span['pipeline_identity'] = 'executable_mismatch' if mismatches else 'live_handle'
        span['pipeline_mismatches'] = mismatches
        span['pipeline_candidates'] = sorted({a['name'] for a in aliases}) if not mismatches else []
        span['pipeline_alias_truncated'] = any(a['truncated'] for a in aliases)
        span['executable_stats'] = shader_stats


def present_stages(records):
    attempts = collections.defaultdict(list)
    damage = collections.defaultdict(list)
    sources = collections.defaultdict(list)
    for rec in records:
        if rec['type'] == 18:
            attempts[rec['t1']].append(rec)
        elif rec['type'] == 20:
            damage[rec['t1']].append(dict(sequence=rec['t2'], source_tag=rec['t3'], x=rec['a'], y=rec['b'], width=rec['c'], height=rec['d'], source_width=rec['cpu0'], source_height=rec['cpu1'], flags=rec['tid2']))
        elif rec['type'] == 19:
            sources[rec['tid']].append(rec)
    for values in sources.values():
        values.sort(key=lambda r: r['t0'])
    source_times = {tid: [r['t0'] for r in values] for tid, values in sources.items()}
    output = []
    for attempt, rows in attempts.items():
        rows.sort(key=lambda r: r['t0'])
        groups = collections.defaultdict(list)
        for row in rows:
            groups[row['a']].append(row)
        sequence = sorted({r['t2'] for r in rows if r['t2']})
        phases = {}
        for name, start, end in [('wait_swap', 1, 2), ('wait_inflight', 3, 4), ('encode', 5, 6),
                                 ('gpu_wait', 7, 8), ('flip_call', 10, 11)]:
            a, b = groups[start], groups[end]
            phases[name] = (b[0]['t0'] - a[0]['t0']) if len(a) == len(b) == 1 and b[0]['t0'] >= a[0]['t0'] else None
        source_records = []
        if len(groups[5]) == 1:
            begin = groups[5][0]
            ends = groups[6] or groups[13] or groups[9]
            if len(ends) == 1:
                source_records = [dict(host_us=r['t0'], write_epoch=r['t1'], receipt_epoch=r['t2'], serial=r['t3'], native=bool(r['a']), width=r['b'], height=r['c'], selected=bool(r['d']))
                                  for r in sources.get(begin['tid'], [])[bisect.bisect_left(source_times.get(begin['tid'], []), begin['t0']):bisect.bisect_right(source_times.get(begin['tid'], []), ends[0]['t0'])]]
        skipped = len(groups[13]) == 1
        required = [s for s in range(1, 13) if not (skipped and s in (6, 7, 8))]
        output.append(dict(attempt=attempt, sequences=sequence, phases_us=phases,
                           no_gpu_copy=skipped, damage=damage.get(attempt, []), sources=source_records,
                           missing_stages=[s for s in required if len(groups[s]) != 1],
                           records=[dict(stage=r['a'], host_us=r['t0'], sequence=r['t2'],
                                         detail=r['t3'], status=r['b']) for r in rows]))
    return output


def pipeline_names(text):
    names = collections.defaultdict(set)
    for line in text.splitlines():
        match = re.match(r'C .*?\bcs=([0-9a-fA-F]{8}) \| c (.*?)(?: \| |$)', line)
        if match and int(match[1], 16):
            names[match[1].lower()].add(match[2])
    return {key: sorted(value) for key, value in names.items()}


def link_submissions(records, submissions):
    """Match host-clock identities in time order, including reused pointer addresses."""
    commits, vulkan, presents = (collections.defaultdict(list) for _ in range(3))
    for rec in records:
        if rec['type'] == 16:
            commits[rec['t1']].append((rec['t0'] * 1000, rec['a']))
        elif rec['type'] == 12 and rec['a'] == 60:
            vulkan[rec['t2']].append((rec['t0'] * 1000, rec['t1']))
        elif rec['type'] == 17:
            presents[rec['t1']].append((rec['t0'] * 1000, rec['t2'],
                                        rec['a'], rec['b'], rec['c'], rec['d']))
    for group in (commits, vulkan, presents):
        for values in group.values():
            values.sort()
    output = []
    for sub in submissions:
        linked = dict(sub, runtime=None, metal_commit=None)
        links = vulkan.get(sub['command'], [])
        at = bisect.bisect_right(links, (sub['host_ns'], 1 << 64)) - 1
        if at >= 0:
            when, runtime = links[at]
            choices = commits.get(runtime, [])
            ci = bisect.bisect_right(choices, (when, 1 << 64)) - 1
            linked.update(runtime=runtime, runtime_link_ns=when)
            if ci >= 0:
                linked['metal_commit'] = choices[ci][1]
                prior = choices[ci - 1][0] if ci else 0
                matching = [p for p in presents.get(runtime, []) if prior < p[0] <= choices[ci][0]]
                if len(matching) == 1:
                    _, sequence, mapping, width, height, path = matching[0]
                    linked['present'] = dict(sequence=sequence, mapping=mapping,
                                             width=width, height=height, path=path)
        output.append(linked)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames', required=True, type=Path)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--pipelines', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--spans-jsonl', type=Path, help='Stream spans separately and retain a compact summary JSON')
    parser.add_argument('--summary-only', action='store_true', help='Parse every completed batch and retain aggregate measurements only')
    args = parser.parse_args()
    if args.output.resolve() in {p.resolve() for p in (args.frames, args.work, args.pipelines) if p}:
        parser.error('output must not replace a capture')
    if args.spans_jsonl and args.spans_jsonl.resolve() in {p.resolve() for p in (args.frames, args.work, args.pipelines, args.output) if p}:
        parser.error('span output must differ from captures and summary')
    records, transport = read_frames(args.frames.read_bytes())
    result = dict(transport=transport, records_by_type=dict(collections.Counter(r['type'] for r in records)),
                  clocks='host_ns is uptime; begin_ns/end_ns are GPU clock; never subtract these domains',
                  present_stages=present_stages(records),
                  export_policy_decisions=[dict(host_us=r['t0'], policy=r['t1'], sample_slots=r['t2'], bytes=r['t3'],
                      changed_ratio_1024=r['a'], variant=r['b'], width=r['c'], height=r['d'],
                      coverage_class=r['cpu0'], completed_updates=r['cpu1'], thread=r['tid'])
                      for r in records if r['type'] == 21])
    spans = iter(())
    journal = None
    linked, by_fence = [], collections.defaultdict(list)
    if args.work:
        meta, submissions = work_report(), []
        with args.work.open(errors='replace') as stream:
            for _ in work_spans(stream, meta, submissions, metadata_only=True):
                pass
        matching = bool(meta['identities']) and all(identity == dict(pid=transport['pid'], epoch=transport['epoch']) for identity in meta['identities'])
        report = work_report()
        report['matches_frame_identity'] = matching
        result['work_transport'] = report
        if args.pipelines:
            text = args.pipelines.read_text(errors='replace')
            identities = re.findall(r'^PROFILE version=1 pid=(\d+) epoch=(\d+)$', text, re.M)
            journal_matches = bool(identities) and all((int(pid), int(epoch)) == (transport['pid'], transport['epoch']) for pid, epoch in identities)
            result['pipeline_identity_matches'] = journal_matches
            if journal_matches:
                journal = PipelineJournal(text)
                result['pipeline_journal'] = dict(callbacks=journal.callbacks,
                    unresolved_aliases=journal.unresolved_aliases, alias_truncations=journal.alias_truncations,
                    rejected_records=journal.rejected_records)
        linked = link_submissions(records, submissions) if matching else []
        for sub in linked:
            key = sub['submission'], sub['queue'], sub['fence'], sub['command']
            by_fence[key].append(sub)
        result.update(submission_links=linked, warning='Durations are inclusive; nested ranges overlap and must not be summed.')
        work_file = args.work.open(errors='replace')
        spans = work_spans(work_file, report)

    aggregates = collections.defaultdict(lambda: dict(count=0, inclusive_ns=0, max_ns=0))
    span_output = args.spans_jsonl.open('w') if args.spans_jsonl else None
    compact = bool(span_output) or args.summary_only
    try:
        with args.output.open('w') as out:
            if not compact:
                out.write('{"gpu_spans":[')
            first = True
            for span in spans:
                fence = span['fence_link']
                host_ns = None
                if fence and span['command'] is not None:
                    key = fence['submission'], fence['queue'], fence['fence'], span['command']
                    choices = by_fence.get(key, [])
                    if len(choices) == 1:
                        sub = choices[0]
                        host_ns = sub['host_ns']
                        span.update(metal_commit=sub['metal_commit'], runtime=sub['runtime'], present=sub.get('present'))
                if journal:
                    journal.annotate(span, host_ns)
                total = aggregates[span['name']]
                total['count'] += 1
                total['inclusive_ns'] += span['duration_ns']
                total['max_ns'] = max(total['max_ns'], span['duration_ns'])
                if span_output:
                    span_output.write(json.dumps(span, separators=(',', ':')) + '\n')
                elif not compact:
                    if not first:
                        out.write(',')
                    json.dump(span, out, separators=(',', ':'))
                first = False
            result['inclusive_by_event'] = dict(aggregates)
            if compact:
                if span_output:
                    result['gpu_spans_jsonl'] = str(args.spans_jsonl)
                json.dump(result, out, indent=2)
            else:
                out.write('],')
                out.write(json.dumps(result, separators=(',', ':'))[1:])
            out.write('\n')
    finally:
        if args.work:
            work_file.close()
        if span_output:
            span_output.close()
    print(json.dumps(transport))


if __name__ == '__main__':
    main()
