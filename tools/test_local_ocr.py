#!/usr/bin/env python3
"""Run the real pinned model on existing synthetic Japanese dialogue fixtures.

--backend ncnn (default): PP-OCRv5 mobile, model directory from prepare_ocr.py.
--backend vocr: vita-vn-ocr, a directory holding the release's H15_w8.vocr
(tools/prepare_ocr.py --backend vocr).
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from PIL import Image, ImageOps

LINES = ('朝ごはん、もう冷めちゃったよ。', '早く起きないと学校に遅刻するってば')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--model', type=Path, required=True)
    ap.add_argument('--backend', choices=('ncnn', 'vocr'), default='ncnn')
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    image = Image.open(root/'host/samples/vn_textbox.jpg').crop((45, 375, 920, 475)).convert('RGBA')
    weights_name = 'PP_OCRv5_mobile_rec.ncnn.bin' if args.backend == 'ncnn' else 'H15_w8.vocr'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pixels = tmp/'input.rgba'
        def run(im, model=args.model):
            pixels.write_bytes(im.tobytes())
            cmd = [args.cli] + (['--backend', 'vocr'] if args.backend == 'vocr' else [])
            return subprocess.run(cmd + [str(model), str(im.width), str(im.height), str(pixels)],
                                  text=True, capture_output=True, timeout=60)
        result = run(image)
        assert result.returncode == 0, result.stderr
        for line in LINES:
            assert line in result.stdout, result.stdout
        assert 'lines=2' in result.stderr, result.stderr
        # Same-size corruption exercises the runtime SHA-256 check, not just length.
        bad = tmp/'bad'; bad.mkdir()
        weights = bytearray((args.model/weights_name).read_bytes())
        weights[len(weights)//2] ^= 1
        (bad/weights_name).write_bytes(weights)
        result = run(image, bad)
        assert result.returncode == 1 and 'rc=-115' in result.stderr and not result.stdout, result
        result = run(image, tmp/'missing')
        assert result.returncode == 1 and 'rc=-115' in result.stderr, result
        result = run(Image.new('RGBA', (960, 100), (0,0,0,255)))
        assert result.returncode == 0 and not result.stdout.strip(), result
        # Full allowed line width and eight lines stress the fixed workspace.
        one = image.crop((0, 0, image.width, 43)).resize((960, 48))
        many = Image.new('RGBA', (960, 544), (0,0,0,255))
        for n in range(8): many.paste(one, (0, n*66))
        result = run(many)
        assert result.returncode == 0 and 'lines=8' in result.stderr, result.stderr
        if args.backend == 'vocr':
            check_vocr(run, image, many)
        print(f'Real-model OCR ({args.backend}), multiline bounds, blank input, and corrupt/missing model checks passed.')


def stat(stderr, key):
    return int(re.search(rf'\b{key}=(\d+)', stderr).group(1))


def check_vocr(run, image, many):
    """What the bright-pixel finder cannot do, and the exact memory bound."""
    # An empty region never reads the weights file.
    result = run(Image.new('RGBA', (960, 100), (0,0,0,255)))
    assert stat(result.stderr, 'loaded') == 0 and stat(result.stderr, 'lines') == 0, result.stderr
    # Dark text on a light box: found as dark lines, inverted, read the same.
    dark = Image.merge('RGBA', (*ImageOps.invert(image.convert('RGB')).split(), image.getchannel('A')))
    result = run(dark)
    assert result.returncode == 0 and 'lines=2' in result.stderr and 'dark=2' in result.stderr, result.stderr
    for line in LINES:
        assert line in result.stdout, result.stdout
    # The workspace is the whole job's memory: weights + max(finder, recognizer
    # arena) + the grey region + the runtime's structs.
    result = run(many)
    used = stat(result.stderr, 'tensor_heap_peak')
    parts = stat(result.stderr, 'weights') + stat(result.stderr, 'scratch') + stat(result.stderr, 'region')
    assert used == stat(result.stderr, 'workspace') and 0 <= used - parts < 16384, result.stderr
    assert used < 2 * 1024 * 1024, result.stderr


if __name__ == '__main__':
    main()
