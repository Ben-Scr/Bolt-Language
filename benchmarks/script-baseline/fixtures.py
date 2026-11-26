"""Frozen v1 workloads. The Python implementations supply independent full-file oracles."""
from collections import deque
import random
from struct import iter_unpack, pack


def navigation():
    rng = random.Random(881)
    width = 128
    grid = bytearray(width * width)
    for y in range(1, width - 1):
        for x in range(1, width - 1):
            wall = (x % 16 == 0 and y % 16 not in (7, 8)) or (y % 16 == 0 and x % 16 not in (7, 8))
            grid[y * width + x] = int(not wall and rng.randrange(32) != 0)
    isolated = 3 + 3 * width
    grid[isolated] = 1
    for step in (1, -1, width, -width):
        grid[isolated + step] = 0
    free = [i for i, cell in enumerate(grid) if cell]
    requests = bytearray()
    outputs = [bytearray(), bytearray()]
    for i in range(256):
        start = rng.choice(free)
        end = isolated if i % 16 == 0 else rng.choice(free)
        requests.extend((start % width, start // width, end % width, end // width))
        # Search backwards from the destination; the undirected map has the same distances.
        queue = deque([(end, 0)])
        visited = {end}
        length = None
        while queue:
            tile, distance = queue.popleft()
            if tile == start:
                length = distance
                break
            for step in (-width, width, -1, 1):
                neighbor = tile + step
                if grid[neighbor] and neighbor not in visited:
                    visited.add(neighbor)
                    queue.append((neighbor, distance + 1))
        for revision, out in enumerate(outputs):
            out.extend(pack("<H", 65535 - revision if length is None else length))
    return grid + requests, outputs


def terrain():
    width = 2048
    source = bytearray((x // 4 + y // 3 + ((x // 21) ^ (y // 27)) * 9) & 255
                       for y in range(width) for x in range(width))
    outputs = [bytearray(len(source)), bytearray(len(source))]
    for y in range(1, width - 1):
        for x in range(1, width - 1):
            i = y * width + x
            rise = abs(source[i-1] - source[i+1]) + abs(source[i-width] - source[i+width])
            for limit, out in zip((32, 33), outputs):
                out[i] = 1 if source[i] < 64 else (3 if rise > limit else 2)
    return source, outputs


def inventory():
    rng = random.Random(419)
    tape = bytearray()
    for _ in range(750000):
        tape.extend(pack("<HBB", rng.randrange(4096), int(rng.randrange(100) < 56), rng.randint(1, 12)))
    outputs = []
    for initial in (24, 25):
        balances = [[initial, 0] for _ in range(4096)]
        for item, withdrawal, units in iter_unpack("<HBB", tape):
            row = balances[item]
            delta = -units if withdrawal else units
            if row[0] + delta >= 0:
                row[0] += delta
            else:
                row[1] += 1
        outputs.append(b"".join(pack("<HH", *row) for row in balances))
    return tape, outputs


CASES = {
    "navigation": (navigation, "int steps = 65535;", "int steps = 65534;"),
    "terrain": (terrain, "int slopeLimit = 32;", "int slopeLimit = 33;"),
    "inventory": (inventory, "stock[i] = 24;", "stock[i] = 25;"),
}


def editor_sample(name, data, answers):
    # The embedded Debug host caps instructions per invocation. Compile the same
    # full script, then execute both hosts against a smaller fixture after timing.
    if name == "navigation":
        grid = data[:128 * 128]
        requests = bytes([3, 3, 3, 3, 3, 3, 7, 7, 7, 7, 8, 7])
        assert grid[7 * 128 + 7] and grid[7 * 128 + 8]
        return grid + requests, [pack("<HHH", 0, marker, 1) for marker in (65535, 65534)]
    if name == "terrain":
        width = 2048
        row = next(i // width for i, (a, b) in enumerate(zip(*answers)) if a != b)
        begin = row * width
        return (data[begin - width:begin + 2 * width],
                [bytes(width) + a[begin:begin + width] + bytes(width) for a in answers])
    sample = data[:4096 * 4]
    outputs = []
    for initial in (24, 25):
        on_hand = [initial] * 4096
        rejects = [0] * 4096
        for item, withdrawal, units in iter_unpack("<HBB", sample):
            if withdrawal and units > on_hand[item]:
                rejects[item] += 1
            else:
                on_hand[item] += -units if withdrawal else units
        outputs.append(b"".join(pack("<HH", a, b) for a, b in zip(on_hand, rejects)))
    return sample, outputs
