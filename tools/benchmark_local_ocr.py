#!/usr/bin/env python3
"""Local OCR backends through vjo-ocr on real dialogue regions: memory and accuracy.

Runs the host vjo-ocr tool (the code SceShell runs, with the same line finder,
workspace and model checks) once per region and backend, in a fresh process,
and records its workspace peak (`tensor_heap_peak`), the process's own peak
RSS (`peak_rss`, VmHWM after exec; including the RGBA input it reads and the
tool's code) and wall time. Accuracy is eval's region-level "concat CER" of
bee-san/vita-vn-ocr's hand-checked sets: every line the backend read, top to
bottom, against every line of the region; a refused region counts all of its
characters as errors; regions with an uncertain line are left out.

The regions, ground truth and normalisation come from a vita-vn-ocr checkout
(eval/sets.py, eval/metrics.py); its screenshots (eval/fetch.py) are game
images and stay local. Example:

    tools/benchmark_local_ocr.py --vjo-ocr build/host/vjo-ocr \\
        --vocr-repo ../vita-vn-ocr --eval-data ../vita-vn-ocr/data/eval \\
        --run ncnn=build/host/ocr-model --run vocr=ocr:H15_w8.vocr --out results.json
"""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import sys
import tempfile
import time

from PIL import Image

STAT = re.compile(r'(\w+)=(-?\d+)')


def run_once(cmd, devnull_in=True):
    """Runs cmd; returns (exit status, stdout, stderr, peak RSS in bytes, seconds)."""
    out_r, out_w = os.pipe()
    err_r, err_w = os.pipe()
    t0 = time.perf_counter()
    pid = os.fork()
    if pid == 0:
        os.dup2(out_w, 1)
        os.dup2(err_w, 2)
        if devnull_in:
            fd = os.open(os.devnull, os.O_RDONLY)
            os.dup2(fd, 0)
        os.closerange(3, 256)
        try:
            os.execv(cmd[0], cmd)
        finally:
            os._exit(127)
    os.close(out_w)
    os.close(err_w)
    out, err = b'', b''
    with os.fdopen(out_r, 'rb') as o, os.fdopen(err_r, 'rb') as e:
        out, err = o.read(), e.read()
    _, status, ru = os.wait4(pid, 0)
    dt = time.perf_counter() - t0
    return os.waitstatus_to_exitcode(status), out.decode('utf-8', 'replace'), err.decode('utf-8', 'replace'), \
        ru.ru_maxrss * 1024, dt


def backends(specs):
    """--run ncnn=DIR | vocr=DIR[:MODEL] -> [(label, argv prefix after the binary, model dir)]"""
    out = []
    for spec in specs:
        name, _, rest = spec.partition('=')
        if name == 'ncnn':
            out.append(('ncnn', ['--backend', 'ncnn'], rest))
        elif name == 'vocr':
            d, _, model = rest.partition(':')
            model = model or 'H15_w8.vocr'
            out.append((f'vocr:{model}', ['--backend', 'vocr', '--model', model], d))
        else:
            raise SystemExit(f'unknown backend {spec}')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--vjo-ocr', required=True)
    ap.add_argument('--vocr-repo', type=Path, required=True, help='vita-vn-ocr checkout (eval/ harness)')
    ap.add_argument('--eval-data', type=Path, required=True, help='its eval screenshots (eval/fetch.py)')
    ap.add_argument('--set', default='vita-test,psp-test')
    ap.add_argument('--run', action='append', required=True, help='ncnn=MODEL_DIR or vocr=MODEL_DIR[:MODEL]')
    ap.add_argument('--out', type=Path)
    args = ap.parse_args()
    sys.path.insert(0, str(args.vocr_repo / 'eval'))
    import metrics  # noqa: E402  (vita-vn-ocr's eval/metrics.py)
    import sets  # noqa: E402

    runs = backends(args.run)
    report = {'vjo_ocr': args.vjo_ocr, 'backends': [r[0] for r in runs], 'sets': {}}
    with tempfile.TemporaryDirectory() as tmp:
        rgba = Path(tmp) / 'region.rgba'
        for name in args.set.split(','):
            res = {label: {'regions': 0, 'refused': 0, 'failed': 0, 'concat_errors': 0, 'concat_chars': 0,
                           'peaks': [], 'rss': [], 'sec': [], 'lines_read': 0, 'per_region': []}
                   for label, _, _ in runs}
            for path, (rx, ry, rw, rh), rows in sets.regions(name, args.eval_data):
                im = Image.open(path).convert('RGBA').crop((rx, ry, rx + rw, ry + rh))
                rgba.write_bytes(im.tobytes())
                gts = sorted(rows, key=lambda g: (g['box'][1], g['box'][0]))
                scored = not any(g.get('uncertain') for g in gts)
                cat_g = ''.join(metrics.norm(g['text']) for g in gts)
                for label, extra, model_dir in runs:
                    r = res[label]
                    code, out, err, rss, sec = run_once([args.vjo_ocr, *extra, model_dir, str(rw), str(rh), str(rgba)])
                    st = {k: int(v) for k, v in STAT.findall(err)}
                    rc = st.get('rc', -999)
                    r['regions'] += 1
                    rss = st.get('peak_rss', rss)
                    r['rss'].append(rss)
                    r['sec'].append(sec)
                    if rc == 0:
                        r['peaks'].append(st.get('tensor_heap_peak', 0))
                        r['lines_read'] += st.get('lines', 0)
                        pred = out
                    else:
                        pred = ''
                        r['refused' if rc == -116 else 'failed'] += 1
                    e = metrics.lev(metrics.norm(pred), cat_g) if scored else None
                    if scored:
                        r['concat_errors'] += e
                        r['concat_chars'] += len(cat_g)
                    r['per_region'].append({'image': Path(path).name, 'region': [rx, ry, rw, rh], 'rc': rc,
                                            'lines': st.get('lines'), 'dark': st.get('dark'),
                                            'peak': st.get('tensor_heap_peak'), 'rss': rss,
                                            'ms': round(sec * 1000, 1), 'errors': e,
                                            'chars': len(cat_g) if scored else None})
            summary = {}
            for label, r in res.items():
                summary[label] = {
                    'regions': r['regions'], 'refused': r['refused'], 'failed': r['failed'],
                    'concat_cer': round(r['concat_errors'] / max(1, r['concat_chars']), 5),
                    'concat_errors': r['concat_errors'], 'concat_chars': r['concat_chars'],
                    'lines_read': r['lines_read'],
                    'workspace_peak_max': max(r['peaks'], default=0),
                    'workspace_peak_median': int(statistics.median(r['peaks'])) if r['peaks'] else 0,
                    'rss_max': max(r['rss'], default=0),
                    'rss_median': int(statistics.median(r['rss'])) if r['rss'] else 0,
                    'ms_median': round(1000 * statistics.median(r['sec']), 1) if r['sec'] else 0,
                    'per_region': r['per_region'],
                }
                s = summary[label]
                print(f"{name:9s} {label:18s} regions {s['regions']:3d} refused {s['refused']:2d} failed {s['failed']:2d} "
                      f"concat CER {s['concat_cer']:.2%} ({s['concat_errors']}/{s['concat_chars']}) "
                      f"workspace peak max {s['workspace_peak_max'] / 2**20:.2f} MiB "
                      f"(median {s['workspace_peak_median'] / 2**20:.2f}) RSS max {s['rss_max'] / 2**20:.1f} MiB "
                      f"{s['ms_median']} ms/region (host)", flush=True)
            report['sets'][name] = summary
    if args.out:
        args.out.write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding='utf-8')


if __name__ == '__main__':
    main()
