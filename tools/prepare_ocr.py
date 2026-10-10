#!/usr/bin/env python3
"""Download the exact local OCR weights; for ncnn, also generate its build headers.

No ONNX/Paddle/Python runtime is needed on the Vita. Run without --ncnn-source
to prepare the model directory to copy to ux0:data/VitaJPOverlay/ocr.

--backend vocr fetches a vita-vn-ocr recognizer (default H15_w8.vocr) from the
bee-san/vita-vn-ocr v0.2.0 release instead. While that repository is private,
the download needs an authenticated GitHub CLI (`gh auth login`) with access to
it; --from takes a copy downloaded by other means. Every file is checked
against its pinned size and SHA-256.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import urllib.request

MODEL_REV = '671ac4a72299a86ddee160131ba88fed748df425'
NCNN_REV = '3798be64cfeeebc0afcfcebfb43ed5c9e1b72e82'
MODEL_BASE = f'https://raw.githubusercontent.com/nihui/ncnn-android-ppocrv5/{MODEL_REV}/app/src/main/assets/'
ASSETS = {
    'PP_OCRv5_mobile_rec.ncnn.bin': (MODEL_BASE + 'PP_OCRv5_mobile_rec.ncnn.bin', 8242276,
        '49d9907a55ba20fa6637f9f788f66ab00793bc8ce57a733a09dbe86b9a2e3db0'),
    'PP_OCRv5_mobile_rec.ncnn.param': (MODEL_BASE + 'PP_OCRv5_mobile_rec.ncnn.param', 20031,
        'f52a6586ac3338d8c350db0c9f3c55ff2ecd3763327f9bc7f0efc091f8a63e74'),
    'ppocrv5_dict.h': (f'https://raw.githubusercontent.com/Tencent/ncnn/{NCNN_REV}/examples/ppocrv5_dict.h', 202891,
        'bb1f547301014b93bd1182af8f472b7df75330e46a392ebdf5c57a8ebcf57c91'),
}


# vita-vn-ocr int8 recognizers (ocr_backend = vocr), release v0.2.0 of
# github.com/bee-san/vita-vn-ocr. The plugin pins the same sizes and hashes
# (core/vocr_ocr.c).
VOCR_REPO = 'bee-san/vita-vn-ocr'
VOCR_TAG = 'v0.2.0'
VOCR_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR_TAG}/'
VOCR_ASSETS = {
    'H15_w8.vocr': (VOCR_BASE + 'H15_w8.vocr', 1155968,
        '316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd'),
    'FL10_w8.vocr': (VOCR_BASE + 'FL10_w8.vocr', 657664,
        '500e0b842e5f0cb326f55e7d991a52ab66dd24977ccc89bd188228aedc825e84'),
    'F20_w8.vocr': (VOCR_BASE + 'F20_w8.vocr', 1558016,
        'f8c1abe4743a71a742e718523f4206f992d3bd4fb69de321997b2bd6f16779c0'),
}


def store(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_bytes(data)
    temp.replace(path)
    return path


def fetch(directory, name, assets=ASSETS):
    url, size, sha = assets[name]
    path = directory / name
    def valid(data):
        return len(data) == size and hashlib.sha256(data).hexdigest() == sha
    if path.exists() and valid(path.read_bytes()):
        return path
    request = urllib.request.Request(url, headers={'User-Agent': 'VitaJPOverlay-model-setup'})
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read(size + 1)
    if not valid(data):
        raise RuntimeError('Incorrect or incomplete download: ' + name)
    return store(path, data)


def fetch_vocr(directory, name, source=None):
    """A release asset: a local copy (--from), else the public download, else
    `gh release download` (private repository or draft release)."""
    url, size, sha = VOCR_ASSETS[name]
    path = directory / name
    def valid(data):
        return len(data) == size and hashlib.sha256(data).hexdigest() == sha
    if path.exists() and valid(path.read_bytes()):
        return path
    if source:
        source = Path(source)
        data = (source / name if source.is_dir() else source).read_bytes()
        if not valid(data):
            raise RuntimeError(f'{source} is not the {VOCR_TAG} {name} (size or SHA-256 differs)')
        return store(path, data)
    try:
        return fetch(directory, name, VOCR_ASSETS)
    except (OSError, RuntimeError) as public_error:
        if not shutil.which('gh'):
            raise RuntimeError(f'{url}: {public_error}. Install the GitHub CLI and run `gh auth login` '
                               f'with access to {VOCR_REPO}, or pass --from.') from public_error
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(['gh', 'release', 'download', VOCR_TAG, '-R', VOCR_REPO, '-p', name, '-D', tmp],
                       check=True, stdin=subprocess.DEVNULL, timeout=300)
        data = (Path(tmp) / name).read_bytes()
    if not valid(data):
        raise RuntimeError('Incorrect or incomplete download: ' + name)
    return store(path, data)


def binary_param(text, layer_types):
    """Numeric subset of the official ncnn2mem format, for this pinned graph.

    Layer IDs follow ncnn_add_layer order, including disabled layers. Every
    graph input must already exist, and array lengths/counts are checked.
    """
    lines = text.strip().splitlines()
    assert lines[0] == '7767517'
    layers, blobs = map(int, lines[1].split())
    assert len(lines) == layers + 2
    out = bytearray(struct.pack('<iii', 7767517, layers, blobs))
    ids = {}
    def integer(n): out.extend(struct.pack('<i', n))
    def number(value):
        out.extend(struct.pack('<f', float(value)) if re.search(r'[.eE]', value) else struct.pack('<i', int(value)))
    for line in lines[2:]:
        words = line.split(); kind, _, nb, nt = words[:4]; nb, nt = int(nb), int(nt)
        integer(layer_types.index(kind)); integer(nb); integer(nt)
        for name in words[4:4+nb]: integer(ids[name])
        for name in words[4+nb:4+nb+nt]:
            assert name not in ids
            ids[name] = len(ids); integer(ids[name])
        for field in words[4+nb+nt:]:
            key, value = field.split('=', 1); key = int(key)
            assert key > -23400  # no strings/custom layers in this graph
            integer(key)
            if key <= -23300:
                values = value.split(','); n = int(values[0]); assert n == len(values)-1
                integer(n)
                for value in values[1:]: number(value)
            else: number(value)
        integer(-233)
    assert len(ids) == blobs
    return bytes(out), ids['in0'], ids['out0']


def generate(directory, ncnn_source):
    types = re.findall(r'^ncnn_add_layer\((\w+)', (ncnn_source/'src/CMakeLists.txt').read_text(), re.M)
    param, input_id, output_id = binary_param((directory/'PP_OCRv5_mobile_rec.ncnn.param').read_text(), types)
    source = (directory/'ppocrv5_dict.h').read_text()
    vocab = [json.loads(s) for s in re.findall(r'^\s*("(?:[^"\\]|\\.)*")\s*,?\s*$', source, re.M)]
    assert len(vocab) == 18383
    vocab.append(' ')  # Paddle CTC's final space; token zero is blank.
    lines = ['// Generated by tools/prepare_ocr.py; do not edit.',
             '// Dictionary: Copyright 2025 Tencent, BSD-3-Clause.',
             '// Model architecture: PaddleOCR, Apache-2.0.',
             '#ifndef VJO_PPOCR_DATA_H', '#define VJO_PPOCR_DATA_H',
             'alignas(64) static const unsigned char vjo_ppocr_param[] = {']
    for i in range(0, len(param), 24): lines.append(','.join(str(x) for x in param[i:i+24])+',')
    lines += ['};', f'static const int vjo_ppocr_input = {input_id};', f'static const int vjo_ppocr_output = {output_id};',
              'static const char *const vjo_ppocr_vocab[] = {']
    lines += [json.dumps(s, ensure_ascii=False)+',' for s in vocab]
    lines += ['};', '#endif', '']
    (directory/'vjo_ppocr_data.h').write_text('\n'.join(lines), encoding='utf-8')
    (directory/'PP_OCRv5_mobile_rec.ncnn.param.bin').write_bytes(param)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--backend', choices=('ncnn', 'vocr'), default='ncnn')
    ap.add_argument('--ncnn-source', type=Path)
    ap.add_argument('--model', choices=sorted(VOCR_ASSETS), default='H15_w8.vocr',
                    help='vocr: which v0.2.0 recognizer (default: H15_w8.vocr)')
    ap.add_argument('--from', dest='source', help='vocr: an already downloaded asset (file or directory)')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.backend == 'vocr':
        fetch_vocr(args.output, args.model, args.source)
        print('Verified', args.model)
        print('Model ready in', args.output, '- copy', args.model, 'to ux0:data/VitaJPOverlay/ocr/')
        return
    names = ASSETS if args.ncnn_source else ['PP_OCRv5_mobile_rec.ncnn.bin']
    for name in names:
        fetch(args.output, name)
        print('Verified', name)
    if args.ncnn_source:
        generate(args.output, args.ncnn_source)
    print('Model ready in', args.output)


if __name__ == '__main__':
    main()
