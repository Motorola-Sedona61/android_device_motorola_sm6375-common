# SPDX-License-Identifier: Apache-2.0

import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile


STOCK_SHA1 = '7fdeaa9ca59db5e43b1761d2e6264e758c197c13'
# Branches into the cave replace these 'mov w8, #0xf3fc' instructions.
SITES = {0x3a418: bytes.fromhex('887f9e52'), 0x3a724: bytes.fromhex('887f9e52')}
# QC2C2DEngine::calcStride jump table slot for YCbCr_420_SP_VENUS:
# 0x27 is the 512-byte stride case, 0x1b the 128-byte one used for UBWC.
STRIDE_SLOT, STRIDE_512, STRIDE_128 = 0x18dcc, 0x27, 0x1b
CAVE = 0x449e0
LIMIT = 0x45000
RX_OFFSET = 0x20000


def _rx_header(data):
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', data, 54)
    rx = None
    for index in range(phcount):
        offset = phoff + index * phsize
        kind, flags = struct.unpack_from('<II', data, offset)
        if kind == 1 and flags == 5:
            assert rx is None
            rx = offset
    assert rx is not None
    assert struct.unpack_from('<QQ', data, rx + 8) == (RX_OFFSET, RX_OFFSET)
    return rx


def patch_nv12(data):
    asm = Path(__file__).with_name('nv12_layout.S')
    with tempfile.TemporaryDirectory() as tmp:
        obj, raw = Path(tmp) / 'layout.o', Path(tmp) / 'layout.bin'
        subprocess.run(['llvm-mc', '-triple=aarch64', '-filetype=obj',
                        str(asm), '-o', str(obj)], check=True)
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text',
                        str(obj), str(raw)], check=True)
        payload = raw.read_bytes()

    assert CAVE < len(payload) <= LIMIT
    original = bytearray(data)
    rx = _rx_header(original)
    old_size = CAVE - RX_OFFSET
    new_size = len(payload) - RX_OFFSET
    if struct.unpack_from('<QQ', original, rx + 32) == (new_size, new_size):
        assert original[CAVE:len(payload)] == payload[CAVE:]
        for site in SITES:
            assert original[site:site + 4] == payload[site:site + 4]
        assert original[STRIDE_SLOT] == STRIDE_128
        original[CAVE:len(payload)] = bytes(len(payload) - CAVE)
        struct.pack_into('<QQ', original, rx + 32, old_size, old_size)
        for site, instruction in SITES.items():
            original[site:site + 4] = instruction
        original[STRIDE_SLOT] = STRIDE_512
    assert hashlib.sha1(original).hexdigest() == STOCK_SHA1
    assert struct.unpack_from('<QQ', original, rx + 32) == (old_size, old_size)
    assert not any(original[CAVE:LIMIT])

    for site, instruction in SITES.items():
        assert original[site:site + 4] == instruction
        original[site:site + 4] = payload[site:site + 4]
    original[STRIDE_SLOT] = STRIDE_128
    original[CAVE:len(payload)] = payload[CAVE:]
    struct.pack_into('<QQ', original, rx + 32, new_size, new_size)
    return bytes(original)


def fix_nv12_layout(_ctx, _file, file_path, *args, **kwargs):
    path = Path(file_path)
    path.write_bytes(patch_nv12(path.read_bytes()))
