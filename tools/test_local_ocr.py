#!/usr/bin/env python3
"""Run the real pinned model on existing synthetic Japanese dialogue fixtures."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--model', type=Path, required=True)
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    image = Image.open(root/'host/samples/vn_textbox.jpg').crop((45, 375, 920, 475)).convert('RGBA')
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pixels = tmp/'input.rgba'
        def run(im, model=args.model):
            pixels.write_bytes(im.tobytes())
            return subprocess.run([args.cli, str(model), str(im.width), str(im.height), str(pixels)],
                                  text=True, capture_output=True, timeout=60)
        result = run(image)
        assert result.returncode == 0, result.stderr
        assert '朝ごはん、もう冷めちゃったよ。' in result.stdout, result.stdout
        assert '早く起きないと学校に遅刻するってば' in result.stdout, result.stdout
        assert 'lines=2' in result.stderr, result.stderr
        # Same-size corruption exercises the runtime SHA-256 check, not just length.
        bad = tmp/'bad'; bad.mkdir()
        weights = bytearray((args.model/'PP_OCRv5_mobile_rec.ncnn.bin').read_bytes())
        weights[len(weights)//2] ^= 1
        (bad/'PP_OCRv5_mobile_rec.ncnn.bin').write_bytes(weights)
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
        print('Real-model OCR, multiline bounds, blank input, and corrupt/missing model checks passed.')


if __name__ == '__main__':
    main()
