from __future__ import annotations

import ctypes
import time
from dataclasses import dataclass
from typing import Optional, Union

from ._controls import AXIS_INDICES, INPUT_COUNT, NAME_TO_INDEX, Axis, Button, _Controls

MAGIC = 0x434C5445
MAPPING_NAME = "Local\\ETS2LA_Controller"
EVENT_NAMES = ("Local\\ETS2LA_Controller_Frame0", "Local\\ETS2LA_Controller_Frame1")

CONTROL_BYTES = 4096
TOTAL_BYTES = 8192
MAX_INPUTS = 400
MAX_CLIENTS = 32
CLIENT_TIMEOUT_MS = 3000
HEARTBEAT_INTERVAL_S = 0.25

# Bump whenever ControlBlock/InputBlock layout or semantics change in any way.
PROTOCOL_VERSION = 1

CTL_MAGIC = 0
CTL_VERSION = 4
CTL_INPUT_COUNT = 8
CTL_QPC_FREQ = 16
CTL_DLL_PID = 24
CTL_DELIVERY_MODE = 64
CTL_STATS_EPOCH = 68
CTL_ACTIVE = 128
CTL_FRAME_INDEX = 136
CTL_EVENTS_SENT = 144
CTL_FRAME_INTERVAL = 160
CTL_APPLIED_SEQ = 168
CTL_LAT_LAST = 192
CTL_LAT_MAX = 200
CTL_LAT_SUM = 208
CTL_LAT_SAMPLES = 216
CTL_CLIENTS = 256
CLIENT_STRIDE = 64

IN_WRITE_QPC = 0
IN_WRITE_SEQ = 8
IN_VALUES = 64
IN_PULSES = 64 + 4 * MAX_INPUTS

DELIVERY_CHANGES = 0
DELIVERY_EVERY_FRAME = 1

SYNCHRONIZE = 0x00100000
FILE_MAP_WRITE = 0x0002
FILE_MAP_READ = 0x0004


def _load_kernel32():
    try:
        k = ctypes.WinDLL("kernel32", use_last_error=True)
    except (AttributeError, OSError):
        return None
    from ctypes import wintypes as wt

    k.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
    k.OpenFileMappingW.restype = wt.HANDLE
    k.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
    k.MapViewOfFile.restype = ctypes.c_void_p
    k.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    k.UnmapViewOfFile.restype = wt.BOOL
    k.OpenEventW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
    k.OpenEventW.restype = wt.HANDLE
    k.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
    k.WaitForSingleObject.restype = wt.DWORD
    k.CloseHandle.argtypes = [wt.HANDLE]
    k.CloseHandle.restype = wt.BOOL
    k.GetTickCount64.argtypes = []
    k.GetTickCount64.restype = ctypes.c_uint64
    k.QueryPerformanceCounter.argtypes = [ctypes.POINTER(ctypes.c_int64)]
    k.QueryPerformanceCounter.restype = wt.BOOL
    k.GetCurrentProcessId.argtypes = []
    k.GetCurrentProcessId.restype = wt.DWORD
    return k


kernel32 = _load_kernel32()


@dataclass(frozen=True)
class Latency:
    """Where the game stands compared to the newest commit()."""

    applied: bool
    """True once the game has read the newest commit()."""
    ms: float
    """applied: delay commit() -> game in milliseconds. Otherwise: how long the commit() has waited so far."""


@dataclass(frozen=True)
class ControllerStats:
    """Snapshot of the plugin state and the latency statistics."""

    active: bool
    """The game currently polls the device (in a drivable session)."""
    plugin_pid: int
    """Process id of the game that loaded the plugin."""
    frame_index: int
    """Game input frames processed since the plugin loaded."""
    events_sent: int
    """Input events handed to the game since the plugin loaded."""
    frame_interval_ms: float
    """Time between the last two game input frames."""
    latency_samples: int
    """commit() calls measured since the last reset_stats()."""
    latency_last_ms: float
    """Delay of the last commit(): commit() until the game read it."""
    latency_avg_ms: float
    """Average commit() -> game delay."""
    latency_max_ms: float
    """Worst commit() -> game delay."""


class Controller(_Controls):
    """Lock-free writer for the ``ets2la_controller.dll`` shared memory.

    Set axes and buttons with the setters, then call commit() once per control loop iteration.
    Nothing reaches the game before commit(). The game reads the values once per rendered frame,
    so the delay of a command is "time until the next game frame" and is reported by latency()/stats().
    """

    def __init__(self):
        if kernel32 is None:
            raise OSError("ets2la_controller only works on Windows")
        self._qpc = ctypes.c_int64()
        self._handle = None
        self._addr = 0
        self._events = [None, None]
        self._entry_addr = 0
        self._my_id = 0
        self._freq = 0
        self._last_frame = 0
        self._last_reconnect = 0.0
        self._last_beat = 0.0
        self.last_error = ""
        self._scratch = (ctypes.c_uint32 * INPUT_COUNT)()
        self._connect()
        if not self._addr:
            self._attach_scratch()

    def connected(self) -> bool:
        return self._addr != 0

    def close(self) -> None:
        """Releases every input and disconnects."""
        if self._addr:
            for i in range(INPUT_COUNT):
                self._v[i] = 0
            self._unregister()
            self._v = self._f = self._p = None
            kernel32.UnmapViewOfFile(self._addr)
            self._addr = 0
        if self._handle:
            kernel32.CloseHandle(self._handle)
            self._handle = None
        for i, e in enumerate(self._events):
            if e:
                kernel32.CloseHandle(e)
                self._events[i] = None
        self._attach_scratch()

    def __enter__(self) -> "Controller":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def set(self, control: Union[Axis, Button], value: Union[float, bool]) -> None:
        """Sets an input. Axes take a float in [-1, 1] (clamped by the plugin), buttons a bool."""
        self._ensure_connected()
        i = int(control)
        if i in AXIS_INDICES:
            self._f[i] = value
        else:
            self._v[i] = 1 if value else 0

    def press(self, button: Button) -> None:
        """Triggers a press input (light toggle, gear up, wiper step, ...) for exactly one game frame.

        Returns immediately, the pulse is sent with the next game frame. Pressing twice before one
        game frame has passed still counts as a single press."""
        self._ensure_connected()
        i = int(button)
        self._p[i] = (self._p[i] + 1) & 0xFFFFFFFF

    def get(self, control: Union[Axis, Button]) -> Union[float, bool]:
        """Reads back the value last set for an input."""
        i = int(control)
        return self._f[i] if i in AXIS_INDICES else self._v[i] != 0

    def set_by_name(self, name: str, value: Union[float, bool]) -> None:
        """Sets an input by its readable name, e.g. ``"steering"`` or ``"wipers_level_2"``."""
        i = NAME_TO_INDEX[name]
        self.set(Axis(i) if i in AXIS_INDICES else Button(i), value)

    def release_all(self) -> None:
        """Sets every axis to 0 and releases every button."""
        self._ensure_connected()
        for i in range(INPUT_COUNT):
            self._v[i] = 0

    def commit(self) -> None:
        """Publishes everything set since the last commit() and stamps it for the latency measurement.

        Also acts as heartbeat: without commit() or heartbeat() for 3 seconds the plugin releases all inputs."""
        if not self._ensure_connected():
            return
        self._w_qpc.value = self.qpc_now()
        self._w_seq.value = (self._w_seq.value + 1) & 0xFFFFFFFFFFFFFFFF
        self._beat()

    def heartbeat(self) -> None:
        """Keeps the connection alive without writing (only needed if you pause commit() for > 1 s)."""
        if self._addr:
            self._beat(force=True)

    def wait_frame(self, timeout_ms: int = 1000) -> bool:
        """Blocks until the game starts its next input frame (or timeout).

        Call commit() right after it to get the lowest possible delay: your values are then used
        in the very next frame. Returns False on timeout or while not connected."""
        if not self._ensure_connected():
            time.sleep(min(timeout_ms, 50) / 1000.0)
            return False
        deadline = time.perf_counter() + timeout_ms / 1000.0
        while True:
            latest = self._c_frame.value
            if latest > self._last_frame:
                self._last_frame = latest
                return True
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                return False
            kernel32.WaitForSingleObject(self._events[(self._last_frame + 1) & 1],
                                         max(1, min(5, int(remaining * 1000 + 0.999))))
            self._beat()

    def latency(self) -> Latency:
        """Status of the newest commit(): applied by the game yet, and how long it took / has waited."""
        if not self._addr:
            return Latency(False, 0.0)
        seq = self._w_seq.value
        if self._c_applied_seq.value >= seq:
            return Latency(True, self.ticks_to_ms(self._u64(CTL_LAT_LAST)))
        return Latency(False, self.ticks_to_ms(self.qpc_now() - self._w_qpc.value))

    def stats(self) -> Optional[ControllerStats]:
        """Plugin state and latency statistics, or None while not connected."""
        if not self._addr:
            return None
        n = self._u64(CTL_LAT_SAMPLES)
        total = self.ticks_to_ms(self._u64(CTL_LAT_SUM))
        return ControllerStats(
            active=bool(self._u32(CTL_ACTIVE)),
            plugin_pid=self._u32(CTL_DLL_PID),
            frame_index=self._u64(CTL_FRAME_INDEX),
            events_sent=self._u64(CTL_EVENTS_SENT),
            frame_interval_ms=self.ticks_to_ms(self._u64(CTL_FRAME_INTERVAL)),
            latency_samples=n,
            latency_last_ms=self.ticks_to_ms(self._u64(CTL_LAT_LAST)),
            latency_avg_ms=total / n if n else 0.0,
            latency_max_ms=self.ticks_to_ms(self._u64(CTL_LAT_MAX)),
        )

    def reset_stats(self) -> None:
        """Resets the min/avg/max latency statistics."""
        if self._addr:
            ctypes.c_uint32.from_address(self._addr + CTL_STATS_EPOCH).value += 1

    def set_delivery_mode(self, every_frame: bool) -> None:
        """False (default): only changes are sent to the game. True: every input every frame (diagnostics)."""
        if self._addr:
            ctypes.c_uint32.from_address(self._addr + CTL_DELIVERY_MODE).value = (
                DELIVERY_EVERY_FRAME if every_frame else DELIVERY_CHANGES)

    def qpc_now(self) -> int:
        """Current QueryPerformanceCounter value."""
        kernel32.QueryPerformanceCounter(ctypes.byref(self._qpc))
        return self._qpc.value

    def ticks_to_ms(self, ticks: int) -> float:
        """Converts QueryPerformanceCounter ticks to milliseconds."""
        return ticks * 1000.0 / self._freq if self._freq else 0.0

    def _u32(self, off: int) -> int:
        return ctypes.c_uint32.from_address(self._addr + off).value

    def _u64(self, off: int) -> int:
        return ctypes.c_uint64.from_address(self._addr + off).value

    def _attach_scratch(self) -> None:
        """While disconnected, writes land in private memory so attribute access keeps working."""
        self._v = self._scratch
        self._f = ctypes.cast(self._scratch, ctypes.POINTER(ctypes.c_float))
        self._p = (ctypes.c_uint32 * INPUT_COUNT)()
        self._w_qpc = ctypes.c_uint64()
        self._w_seq = ctypes.c_uint64()

    def _fail(self, msg: str) -> bool:
        self.last_error = msg
        return False

    def _ensure_connected(self) -> bool:
        if self._addr:
            return True
        now = time.perf_counter()
        if now - self._last_reconnect < 0.25:
            return False
        self._last_reconnect = now
        pending = [self._v[i] for i in range(INPUT_COUNT)]
        if self._connect():
            for i, val in enumerate(pending):
                self._v[i] = val
            return True
        return False

    def _connect(self) -> bool:
        h = kernel32.OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, False, MAPPING_NAME)
        if not h:
            return self._fail("controller not running (OpenFileMapping failed). Is ets2la_controller.dll loaded?")
        addr = kernel32.MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, TOTAL_BYTES)
        if not addr:
            kernel32.CloseHandle(h)
            return self._fail("MapViewOfFile failed")
        if ctypes.c_uint32.from_address(addr + CTL_MAGIC).value != MAGIC:
            kernel32.UnmapViewOfFile(addr)
            kernel32.CloseHandle(h)
            return self._fail("controller DLL not ready")
        if ctypes.c_uint32.from_address(addr + CTL_VERSION).value != PROTOCOL_VERSION:
            kernel32.UnmapViewOfFile(addr)
            kernel32.CloseHandle(h)
            return self._fail("controller DLL protocol version mismatch")
        events = [kernel32.OpenEventW(SYNCHRONIZE, False, n) for n in EVENT_NAMES]
        if not all(events):
            for e in events:
                if e:
                    kernel32.CloseHandle(e)
            kernel32.UnmapViewOfFile(addr)
            kernel32.CloseHandle(h)
            return self._fail("OpenEvent failed")

        self._handle, self._addr, self._events = h, addr, events
        base = addr + CONTROL_BYTES
        self._v = (ctypes.c_uint32 * MAX_INPUTS).from_address(base + IN_VALUES)
        self._f = (ctypes.c_float * MAX_INPUTS).from_address(base + IN_VALUES)
        self._p = (ctypes.c_uint32 * MAX_INPUTS).from_address(base + IN_PULSES)
        self._w_qpc = ctypes.c_uint64.from_address(base + IN_WRITE_QPC)
        self._w_seq = ctypes.c_uint64.from_address(base + IN_WRITE_SEQ)
        self._c_frame = ctypes.c_uint64.from_address(addr + CTL_FRAME_INDEX)
        self._c_applied_seq = ctypes.c_uint64.from_address(addr + CTL_APPLIED_SEQ)
        self._freq = ctypes.c_uint64.from_address(addr + CTL_QPC_FREQ).value
        self._register()
        self._last_frame = self._c_frame.value
        self.last_error = ""
        return True

    _counter = 0

    def _register(self) -> None:
        Controller._counter += 1
        pid = kernel32.GetCurrentProcessId()
        self._my_id = (pid << 32) | (Controller._counter & 0xFFFFFFFF)
        now = kernel32.GetTickCount64()
        start = pid % MAX_CLIENTS
        chosen = start
        for i in range(MAX_CLIENTS):
            idx = (start + i) % MAX_CLIENTS
            base = self._addr + CTL_CLIENTS + idx * CLIENT_STRIDE
            cid = ctypes.c_uint64.from_address(base).value
            hb = ctypes.c_uint64.from_address(base + 8).value
            if cid == 0 or now - hb > CLIENT_TIMEOUT_MS * 2:
                chosen = idx
                break
        self._entry_addr = self._addr + CTL_CLIENTS + chosen * CLIENT_STRIDE
        self._c_hb_id = ctypes.c_uint64.from_address(self._entry_addr)
        self._c_hb_ms = ctypes.c_uint64.from_address(self._entry_addr + 8)
        self._beat(force=True)

    def _beat(self, force: bool = False) -> None:
        if not self._entry_addr:
            return
        now = time.perf_counter()
        if not force and now - self._last_beat < HEARTBEAT_INTERVAL_S:
            return
        self._last_beat = now
        self._c_hb_id.value = self._my_id
        self._c_hb_ms.value = kernel32.GetTickCount64()

    def _unregister(self) -> None:
        if self._entry_addr and self._addr and self._c_hb_id.value == self._my_id:
            self._c_hb_ms.value = 0
            self._c_hb_id.value = 0
        self._entry_addr = 0