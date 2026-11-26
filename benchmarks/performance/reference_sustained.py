"""Independently compute the checksums for suite.py --sustained.

Run with Python 3. NumPy is optional and accelerates the 240-million-step FIFO.
No Bolt or C compiler is required. Results are printed as JSON.
"""
import json
import struct

from suite import SUSTAINED_CASES


def fifo_checksum():
    depth = 4096
    steps = 240_000_000
    try:
        import numpy as np
    except ImportError:
        from collections import deque
        queue = deque(i * 7 for i in range(depth))
        total = 0
        for turn in range(steps):
            value = queue.popleft()
            total += value
            queue.append(((value << 7) ^ (value >> 13) ^ (turn * 17)) & 65535)
        return total + queue[0] + depth

    # A complete batch reads the previous queue exactly once before replacing it.
    offsets = np.arange(depth, dtype=np.uint32)
    queue = offsets * 7
    total = 0
    for turn in range(0, steps, depth):
        length = min(depth, steps - turn)
        old = queue[:length]
        total += int(np.sum(old, dtype=np.uint64))
        added = ((old << 7) ^ (old >> 13) ^ ((turn + offsets[:length]) * 17)) & 65535
        queue = added if length == depth else np.concatenate((queue[length:], added))
    return total + int(queue[0]) + depth


def list_search_checksum():
    # In each period, odd multipliers permute all indices and all needles.
    length = 8192
    all_hits = length * (length - 1) // 2
    all_misses = -length
    return (all_hits + all_misses) * (1_048_576 // (2 * length))


def utf8_checksum():
    text = "evt:ä/😀\n"
    unit = text.encode("utf-8")
    contents = bytearray(unit * 4096)
    total = 0
    for step in range(49_152):
        position = ((step * 43) & 4095) * len(unit)
        contents[position] = 65 + (step & 15)
        total += len(text) * 4096 + contents[position]
    return total + sum(contents)


def decimal_checksum():
    literals = ("-1513.125", "0.125", "3.4028234e30", "-1.1754944e-30",
                "2.2250738585072014", "17.00000095367431640625",
                "0.10000000149011612", "-2345.6789012345678")
    combined = []
    for text in literals:
        value = float(text)
        bits64, = struct.unpack(">Q", struct.pack(">d", value))
        bits32, = struct.unpack(">I", struct.pack(">f", value))
        combined.append(bits64 ^ (bits32 << 24))
    result = 17
    for index in range(3_000_031):
        rotated = ((result << 7) & 0xffffffffffffffff) | (result >> 57)
        result = rotated ^ combined[(index ^ (index >> 3)) & 7] ^ index
    return result


def equality_checksum():
    result, index, end = 1, 0, 48_000_000
    seen = {}
    while index < end:
        # Future states depend only on these two values. Skip complete cycles.
        state = (result, index & 127)
        if state in seen:
            period = index - seen[state]
            cycles = (end - index) // period
            if cycles:
                index += cycles * period
                seen.clear()
                continue
        seen[state] = index
        element = (result + (index & 127)) & 127
        equal = element == 0 or (element & 3) >= 2
        result = ((result << 3) ^ (result >> 18) ^ (17 if equal else 51)) & 16777215
        index += 1
    return result


def wide_copy_checksum():
    # Every 1024 passes have the same sequence of copies. Track the original
    # row and the XOR accumulated in its first field across one such period.
    size = 1024
    source = list(range(size))
    mask = [0] * size
    for step in range(size):
        for row in range(size):
            other = (row + step) % size
            source[row] = source[other]
            mask[row] = mask[other] ^ row

    rows = [(i, i + 1) for i in range(size)]
    periods, tail = divmod(262147, size)
    for _ in range(periods):
        rows = [(rows[j][0], rows[j][1] ^ delta) for j, delta in zip(source, mask)]
    for step in range(tail):
        for row in range(size):
            original, first = rows[(row + step) % size]
            rows[row] = (original, first ^ row)
    return sum(first + 11 * original + sum(range(2, 13)) for original, first in rows)


def struct_buffer_checksum():
    y = [(index * 317) & 65535 for index in range(16384)]
    total = 0
    for frame in range(65_536):
        first = (frame * 31) & 8191
        changed = (frame * 43) & 8191
        total += (((first + changed) * 619) & 65535) + frame
        for index in range(32):
            element = first + ((frame * 13 + index * 977) & 8191)
            total += y[element] + ((element * 3) & 255) + element
        y[first + changed] ^= frame
    return total


def zeroed_struct_checksum():
    total = 0
    for frame in range(4096):
        slots = {}
        cursor = frame + 1
        for index in range(4096):
            key = (cursor ^ (index * 77)) & 65535
            owner, flags, previous = slots.get(key, (0, 0, 0))
            cursor = ((cursor << 3) ^ (cursor >> 13) ^ owner ^ flags ^
                      previous ^ index ^ 160171) & 16777215
            slots[key] = (index + frame + 1, cursor & 255, index)
        total += cursor + slots.get(frame, (0, 0, 0))[2]
    return total


if __name__ == "__main__":
    fifo = fifo_checksum()
    results = {
        "queue-worklist": fifo,
        "queue-worklist-ring": fifo,
        "list-search-simd-unroll4": list_search_checksum(),
        "utf8-roundtrip-simd": utf8_checksum(),
        "float-parse": decimal_checksum(),
        "string-equality": equality_checksum(),
        "struct-buffer": struct_buffer_checksum(),
        "struct-copy-wide": wide_copy_checksum(),
        "struct-zero": zeroed_struct_checksum(),
    }
    print(json.dumps(results, indent=2))
    if {name: str(value) for name, value in results.items()} != SUSTAINED_CASES:
        raise SystemExit("A computed checksum differs from the suite.")
