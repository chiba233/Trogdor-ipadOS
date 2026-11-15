#!/usr/bin/env python3
"""Generate the camera driver's register tables from pinned upstream files (upstream.lock.json linux-camera, chromiumos-camx).

The tables are GPL sources' data, so the repository carries none of them: this fetches each pinned file into the ignored
upstream/camera/, checks its git blob hash, and writes coachz_camera_tables.h next to the driver in a build copy. What is taken:
  ov8856.c: mipi_data_rate_lane_4's second list (360 Mbps a lane, link 180 MHz), lane_4_mode_1640x1232 with its hts, vts and
            link index from lane_cfg_4, and mipi_data_mbus_sgrbg10_1x10.
  cam_csiphy_1_2_2_hwreg.h: the common, reset and interrupt lists, the 2-phase per-lane lists and the 2-phase list length.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'upstream/camera'
OV8856 = 'drivers/media/i2c/ov8856.c'
CSIPHY = 'drivers/media/platform/camx/cam_sensor_module/cam_csiphy/include/cam_csiphy_1_2_2_hwreg.h'
PHY_TYPES = ('DEFAULT_PARAMS', 'LANE_ENABLE', 'SETTLE_CNT_LOWER_BYTE', 'SETTLE_CNT_HIGHER_BYTE', 'DNP_PARAMS', '2PH_REGS', '3PH_REGS')


def blob(data):
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


def pinned(source, path):
    spec = json.loads((ROOT / 'upstream.lock.json').read_text())['sources'][source]
    want = spec['files'][path]
    local = CACHE / source / path
    if not local.is_file():
        url = f"{spec['url']}/+/{spec['commit']}/{path}?format=TEXT"
        request = urllib.request.Request(url, headers={'User-Agent': 'coachz-camera-tables'})
        with urllib.request.urlopen(request, timeout=60) as response:
            data = base64.b64decode(response.read())
        if blob(data) != want:
            raise ValueError(f'{path}: blob {blob(data)} is not the pinned {want}')
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_bytes(data)
    data = local.read_bytes()
    if blob(data) != want:
        raise ValueError(f'{local}: cached file differs from the pinned blob {want}')
    return data.decode()


def block(text, opener):
    """The brace block that starts at the first '{' after opener."""
    start = text.index(opener)
    at = text.index('{', start)
    depth = 0
    for i in range(at, len(text)):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        if depth == 0:
            return text[at + 1:i]
    raise ValueError('unterminated block: ' + opener)


def groups(body):
    """The top-level brace groups inside a block."""
    out, depth, start = [], 0, None
    for i, c in enumerate(body):
        if c == '{':
            if depth == 0:
                start = i + 1
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                out.append(body[start:i])
    return out


def reg_pairs(body):
    pairs = [(int(a, 16), int(v, 16)) for a, v in re.findall(r'\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}', body)]
    if not pairs or any(a > 0xffff or v > 0xff for a, v in pairs):
        raise ValueError('unexpected sensor register list')
    return pairs


def phy_entries(body):
    rows = re.findall(r'\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*CSIPHY_([A-Z0-9_]+)\s*\}', body)
    entries = [(int(a, 16), int(v, 16), int(d, 16), t) for a, v, d, t in rows]
    if not entries or any(t not in PHY_TYPES or a > 0xffff or v > 0xff or d > 0xff for a, v, d, t in entries):
        raise ValueError('unexpected CSIPHY register list')
    return entries


def ov8856_tables(text):
    rates = groups(block(text, 'static const struct ov8856_mipi_data_rates mipi_data_rate_lane_4 ='))
    if len(rates) != 2 or '360mbps' not in text[text.index('mipi_data_rate_lane_4 ='):][:600]:
        raise ValueError('mipi_data_rate_lane_4 changed shape')
    mode = reg_pairs(block(text, 'static const struct ov8856_reg lane_4_mode_1640x1232[] ='))
    cfg = block(text, 'static const struct ov8856_lane_cfg lane_cfg_4 =')
    meta = next(g for g in groups(groups(cfg)[2]) if 'lane_4_mode_1640x1232' in g)
    field = {k: int(v, 0) for k, v in re.findall(r'\.(width|height|hts|vts_def|link_freq_index|data_lanes)\s*=\s*(\w+)', meta)}
    if (field['width'], field['height'], field['data_lanes'], field['link_freq_index']) != (1640, 1232, 4, 1):
        raise ValueError('lane_4_mode_1640x1232 metadata changed')
    freqs = [int(v) for v in re.findall(r'(\d{9})', groups(cfg)[0])]
    if freqs != [360000000, 180000000]:
        raise ValueError('lane_cfg_4 link frequencies changed')
    bayer = reg_pairs(block(text, 'static const struct ov8856_reg mipi_data_mbus_sgrbg10_1x10[] ='))
    return reg_pairs(rates[1]), mode, bayer, field


def csiphy_tables(text):
    parms = block(text, 'struct csiphy_reg_parms_t csiphy_v1_2_2 =')
    size = int(re.search(r'\.csiphy_2ph_config_array_size\s*=\s*(\d+)', parms).group(1))
    common = phy_entries(block(text, 'struct csiphy_reg_t csiphy_common_reg_1_2_2[] ='))
    reset = phy_entries(block(text, 'struct csiphy_reg_t csiphy_reset_reg_1_2_2[] ='))
    irq = phy_entries(block(text, 'struct csiphy_reg_t csiphy_irq_reg_1_2_2[] ='))
    lanes = [phy_entries(g) for g in groups(block(text, 'csiphy_reg_t csiphy_2ph_v1_2_2_reg[MAX_LANES][MAX_SETTINGS_PER_LANE] ='))]
    if len(lanes) != 5 or any(len(lane) < size for lane in lanes) or len(irq) != 11:
        raise ValueError('CSIPHY 1.2.2 tables changed shape')
    return common, reset, irq, [lane[:size] for lane in lanes], size


def header(ov, phy):
    pll, mode, bayer, field = ov
    common, reset, irq, lanes, size = phy
    spec = json.loads((ROOT / 'upstream.lock.json').read_text())['sources']
    out = ['/* Generated by tools/camera_tables.py; not tracked. Data of GPL-2.0 sources:',
           f" *   {OV8856} at {spec['linux-camera']['commit']}",
           f" *   {CSIPHY} at {spec['chromiumos-camx']['commit']} */",
           '#pragma once', '']
    def sensor(name, pairs):
        out.append(f'static const struct cam_sensor_reg {name}[{len(pairs)}] = {{')
        out.extend(f'\t{{0x{a:04x}, 0x{v:02x}}},' for a, v in pairs)
        out.append('};')
    sensor('ov8856_pll_360mbps', pll)
    sensor('ov8856_mode_1640x1232', mode)
    sensor('ov8856_bayer_sgrbg10', bayer)
    out += [f"#define OV8856_MODE_WIDTH {field['width']}u", f"#define OV8856_MODE_HEIGHT {field['height']}u",
            f"#define OV8856_MODE_HTS {field['hts']}u", f"#define OV8856_MODE_VTS {field['vts_def']}u",
            '#define OV8856_LINK_HZ 180000000ull', '']
    def phy(entries):
        return ', '.join(f'{{0x{a:04x}, 0x{v:02x}, {d}, CAM_PHY_{t}}}' for a, v, d, t in entries)
    for name, entries in (('csiphy_common', common), ('csiphy_reset', reset), ('csiphy_irq', irq)):
        out.append(f'static const struct cam_phy_reg {name}[{len(entries)}] = {{{phy(entries)}}};')
    out.append(f'#define CSIPHY_2PH_SIZE {size}')
    out.append(f'static const struct cam_phy_reg csiphy_2ph[5][CSIPHY_2PH_SIZE] = {{')
    out.extend('\t{' + phy(lane) + '},' for lane in lanes)
    out.append('};')
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path, help='the header to write')
    args = parser.parse_args()
    text = header(ov8856_tables(pinned('linux-camera', OV8856)), csiphy_tables(pinned('chromiumos-camx', CSIPHY)))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)


if __name__ == '__main__':
    main()
