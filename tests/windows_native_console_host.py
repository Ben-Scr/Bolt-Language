#!/usr/bin/env python3
"""Private worker for windows_native_console.py; run in its hidden new console."""
from __future__ import annotations

import ctypes
from ctypes import wintypes as w
import os
from pathlib import Path
import subprocess
import sys
import time


class Coord(ctypes.Structure):
    _fields_ = [('X', w.SHORT), ('Y', w.SHORT)]


class SmallRect(ctypes.Structure):
    _fields_ = [('Left', w.SHORT), ('Top', w.SHORT),
                ('Right', w.SHORT), ('Bottom', w.SHORT)]


class ScreenInfo(ctypes.Structure):
    _fields_ = [('size', Coord), ('cursor', Coord), ('attributes', w.WORD),
                ('window', SmallRect), ('maximum', Coord)]


class KeyEvent(ctypes.Structure):
    _fields_ = [('down', w.BOOL), ('repeat', w.WORD), ('key', w.WORD),
                ('scan', w.WORD), ('character', w.WCHAR), ('state', w.DWORD)]


class InputEvent(ctypes.Union):
    _fields_ = [('key', KeyEvent), ('padding', ctypes.c_byte * 16)]


class InputRecord(ctypes.Structure):
    _fields_ = [('kind', w.WORD), ('event', InputEvent)]


def main() -> int:
    import msvcrt

    screen_exe, input_exe = map(Path, sys.argv[1:])
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)

    def bind(name, result, *parameters):
        function = getattr(kernel, name)
        function.restype = result
        function.argtypes = parameters
        return function

    pointer = ctypes.POINTER
    create_file = bind('CreateFileW', w.HANDLE, w.LPCWSTR, w.DWORD, w.DWORD,
                       w.LPVOID, w.DWORD, w.DWORD, w.HANDLE)
    info = bind('GetConsoleScreenBufferInfo', w.BOOL, w.HANDLE, pointer(ScreenInfo))
    get_mode = bind('GetConsoleMode', w.BOOL, w.HANDLE, pointer(w.DWORD))
    set_mode = bind('SetConsoleMode', w.BOOL, w.HANDLE, w.DWORD)
    set_cursor = bind('SetConsoleCursorPosition', w.BOOL, w.HANDLE, Coord)
    set_attributes = bind('SetConsoleTextAttribute', w.BOOL, w.HANDLE, w.WORD)
    fill_text = bind('FillConsoleOutputCharacterW', w.BOOL, w.HANDLE, w.WCHAR,
                     w.DWORD, Coord, pointer(w.DWORD))
    fill_attributes = bind('FillConsoleOutputAttribute', w.BOOL, w.HANDLE, w.WORD,
                           w.DWORD, Coord, pointer(w.DWORD))
    read_text = bind('ReadConsoleOutputCharacterW', w.BOOL, w.HANDLE, w.LPWSTR,
                     w.DWORD, Coord, pointer(w.DWORD))
    read_attributes = bind('ReadConsoleOutputAttribute', w.BOOL, w.HANDLE,
                           pointer(w.WORD), w.DWORD, Coord, pointer(w.DWORD))
    write_input = bind('WriteConsoleInputW', w.BOOL, w.HANDLE, pointer(InputRecord),
                       w.DWORD, pointer(w.DWORD))
    flush_input = bind('FlushConsoleInputBuffer', w.BOOL, w.HANDLE)

    def checked(success):
        if not success:
            raise ctypes.WinError(ctypes.get_last_error())

    def console_file(name, mode):
        handle = create_file(name, 0xC0000000, 3, None, 3, 0, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        descriptor = msvcrt.open_osfhandle(handle, os.O_BINARY | mode)
        return os.fdopen(descriptor, 'rb' if mode == os.O_RDONLY else 'wb', buffering=0)

    # The parent starts this worker with CREATE_NEW_CONSOLE and SW_HIDE. Opening
    # the console devices bypasses this worker's captured standard-output pipe.
    with console_file('CONIN$', os.O_RDONLY) as cin, console_file('CONOUT$', os.O_WRONLY) as cout:
        hin, hout = msvcrt.get_osfhandle(cin.fileno()), msvcrt.get_osfhandle(cout.fileno())
        mode = w.DWORD()
        checked(get_mode(hin, ctypes.byref(mode)))
        checked(set_mode(hin, (mode.value | 3) & ~4))  # Line input without echo.
        checked(flush_input(hin))
        state = ScreenInfo()
        checked(info(hout, ctypes.byref(state)))
        cells = state.size.X * state.size.Y
        count = w.DWORD()
        checked(fill_text(hout, '#', cells, Coord(0, 0), ctypes.byref(count)))
        checked(fill_attributes(hout, 0x70, cells, Coord(0, 0), ctypes.byref(count)))
        checked(set_attributes(hout, 0x1E))
        checked(set_cursor(hout, Coord(0, 0)))

        def screen(length=cells, offset=0):
            buffer = ctypes.create_unicode_buffer(length)
            used = w.DWORD()
            checked(read_text(hout, buffer, length, Coord(offset % state.size.X, offset // state.size.X),
                              ctypes.byref(used)))
            return buffer[:used.value]

        def type_line(text):
            encoded = (text + '\r').encode('utf-16-le')
            records = (InputRecord * (len(encoded) // 2))()
            for index, record in enumerate(records):
                unit = int.from_bytes(encoded[index * 2:index * 2 + 2], 'little')
                record.kind = 1
                record.event.key = KeyEvent(1, 1, 13 if unit == 13 else 0,
                                            28 if unit == 13 else 0, chr(unit), 0)
            used = w.DWORD()
            checked(write_input(hin, records, len(records), ctypes.byref(used)))
            assert used.value == len(records), 'Not all console input events were written'

        def wait_for(process, prefix, offset=0):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                # The console counts screen cells, while Unicode characters can
                # occupy two cells depending on the console font/code page.
                if screen(len(prefix) * 2, offset).startswith(prefix):
                    return
                if process.poll() is not None:
                    raise AssertionError(f'Console process exited before {prefix!r}: {process.stderr.read()!r}')
                time.sleep(0.01)
            short = screen(len(prefix), offset)
            wide = screen(80, offset)
            raise AssertionError(f'Console output did not become {ascii(prefix)}; '
                                 f'short={ascii(short)}, codes={[ord(c) for c in short]}; '
                                 f'wide={ascii(wide)}, codes={[ord(c) for c in wide[:20]]}')

        def assert_cleared(prefix):
            assert screen() == prefix + ' ' * (cells - len(prefix)), 'Clear did not erase the screen buffer'
            current = ScreenInfo()
            checked(info(hout, ctypes.byref(current)))
            assert (current.cursor.X, current.cursor.Y) == (len(prefix), 0), 'Clear did not reset cursor'
            attributes = (w.WORD * cells)()
            checked(read_attributes(hout, attributes, cells, Coord(0, 0), ctypes.byref(count)))
            assert count.value == cells and all(value == 0x1E for value in attributes), (
                'Clear did not preserve the current console text attributes')

        process = subprocess.Popen([str(screen_exe)], stdin=cin, stdout=cout,
                                   stderr=subprocess.PIPE, cwd=screen_exe.parent)
        try:
            wait_for(process, 'Grüße Ω')
            wait_for(process, 'next', state.size.X)
            checked(info(hout, ctypes.byref(state)))
            assert (state.cursor.X, state.cursor.Y) == (4, 1), (
                'WriteLine did not begin the next console line in column zero')
            type_line('next')
            wait_for(process, 'cleared')
            assert_cleared('cleared')
            type_line('finish')
            _, stderr = process.communicate(timeout=15)
            assert process.returncode == 0 and not stderr, (process.returncode, stderr)
            assert_cleared('')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        print('PASS real console WriteConsoleW and Clear screen/cursor/attributes', flush=True)

        checked(flush_input(hin))
        process = subprocess.Popen([str(input_exe)], stdin=cin, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, cwd=input_exe.parent)
        try:
            lines = ['Grüße 😀', '日本語', '', 'final']
            for line in lines:
                type_line(line)
            stdout, stderr = process.communicate(timeout=15)
            expected = ''.join('[' + line + ']\n' for line in lines).encode('utf-8')
            assert process.returncode == 0 and not stderr, (process.returncode, stderr)
            assert stdout == expected, f'Console Unicode input mismatch: {stdout!r} != {expected!r}'
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        print('PASS real console ReadConsoleW Unicode, surrogate pair and empty line', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
