"""Deterministic, independent input and output oracles for the transfer benchmarks."""
import hashlib
import random
import struct


def telemetry(small):
    rng = random.Random(10817)
    rows = [(rng.randrange(64), rng.choice([200, 200, 200, 304, 404, 500, 503]),
             rng.choice([250, rng.randrange(1, 1600)])) for _ in range(64 if small else 48000)]
    tape = ''.join(f'{sid},{status},{ms}\n' for sid, status, ms in rows).encode('ascii')
    answers = []
    for inclusive in (True, False):
        counts = [[0, 0, 0] for _ in range(64)]
        for sid, status, ms in rows:
            c = counts[sid]
            c[0] += 1
            c[1] += status >= 500
            c[2] += ms > 250 or (inclusive and ms == 250)
        answers.append(b''.join(struct.pack('<III', *c) for c in counts))
    return tape, answers


def atlas(small):
    rng = random.Random(9127)
    size = 256 if small else 512 * 512
    colors = [rng.randbytes(3) for _ in range(256)]
    pixels = []
    stream = bytearray()
    while len(pixels) < size:
        count = min(rng.randint(1, 64), size - len(pixels))
        if rng.randrange(3):
            color = rng.choice([7, 8, rng.randrange(256)])
            stream.extend([128 + count - 1, color])
            pixels.extend([color] * count)
        else:
            indices = rng.randbytes(count)
            stream.append(count - 1)
            stream.extend(indices)
            pixels.extend(indices)
    data = size.to_bytes(3, 'little') + b''.join(colors) + stream
    answers = []
    for transparent in (7, 8):
        rgba = [rgb + bytes([0 if i == transparent else 255]) for i, rgb in enumerate(colors)]
        answers.append(b''.join(rgba[p] for p in pixels))
    return data, answers


def timeline(small):
    rng = random.Random(4403)
    times = [rng.randrange(24 if small else 2048) for _ in range(65 if small else 24577)]
    def encode(values):
        return b''.join(struct.pack('<H', v) for v in values)
    return encode(times), [encode(sorted(range(len(times)), key=lambda i: (times[i], sign*i)))
                           for sign in (1, -1)]


CASES = {
    'telemetry': (telemetry, 'latency >= 250', 'latency > 250'),
    'atlas': (atlas, 'int transparent = 7;', 'int transparent = 8;'),
    'timeline': (timeline, 'times[source[left]] <= times[source[right]]',
                 'times[source[left]] < times[source[right]]'),
}


def digests(data, outputs):
    return {'input_bytes': len(data), 'input_sha256': hashlib.sha256(data).hexdigest(),
            'output_sha256': [hashlib.sha256(a).hexdigest() for a in outputs]}


if __name__ == '__main__':
    import json
    print(json.dumps({name: {label: digests(*generate(small)) for label, small in
                                  [('full', False), ('editor', True)]}
                      for name, (generate, _, _) in CASES.items()}, indent=2))
