#!/usr/bin/env python3
"""
deckd - the PC daemon for the DIY Stream Deck (Windows 10/11).

How the data flows:
	button -> STM32 -> ESP32 --MQTT--> deckd --keybd_event--> active application
																				^
											deckctl --IPC (TCP 127.0.0.1)

The device only reports events. All binding logic lives here.

Requirements: Python 3.10+, paho-mqtt >= 2.0, PyYAML. Win32 is used only via ctypes.
Run it in the interactive user session (not as a Windows service), because
SetWinEventHook and key emulation do not work from a service session.

Environment variables (useful for tests):
  DECK_CONFIG - path to config.yaml (default: %APPDATA%\\deck\\config.yaml)
"""

from __future__ import annotations

import copy
import ctypes
import ctypes.wintypes as wt
import json
import logging
import os
import queue
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt
import yaml

# The Win32 parts are loaded only on Windows, so the rest of the file can be tested elsewhere.
IS_WIN = sys.platform == "win32"
log = logging.getLogger("deckd")

#
# Constants
#

# Event types a binding can listen to.
ON_TYPES = ("press", "release", "click", "long")

# The "event" field sent by the ESP32 -> our event type (spec 6.1).
EVENT_MAP = {
  "pressed": "press",
  "released": "release",
  "clicks": "click",
  "long_press": "long",
}

DEFAULTS = {
  # Must be the same as CONFIG_APP_MQTT_BASE_TOPIC on the ESP32 (see Kconfig.projbuild).
  "base_topic": "stm32bridge",
  "mqtt": {"host": "127.0.0.1", "port": 1883, "username": None, "password": None},
  "ipc_port": 47800,
  "learn_timeout": 15,  # seconds to wait for a button in learn mode
  "apps": [],
}

# Alt+Tab creates many window events in a row, so we wait until they stop (FR-W3).
WINDOW_DEBOUNCE_S = 0.15

# After learn mode, a "click" event can arrive up to ~300 ms after the release.
SUPPRESS_AFTER_RELEASE_S = 0.5


def default_config_path() -> Path:
  """Return %APPDATA%\\deck\\config.yaml, or the path from DECK_CONFIG."""
  env = os.environ.get("DECK_CONFIG")
  if env:
    return Path(env)
  base = os.environ.get("APPDATA") or str(Path.home())
  return Path(base) / "deck" / "config.yaml"


#
# Keys and actions (FR-D2, FR-D5, FR-D6)
#

# Windows virtual-key codes for the supported named keys.
VK = {
  "ctrl": 0x11, "shift": 0x10, "alt": 0x12, "win": 0x5B,
  "enter": 0x0D, "tab": 0x09, "esc": 0x1B, "space": 0x20,
  "backspace": 0x08, "delete": 0x2E, "insert": 0x2D,
  "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28,
  "home": 0x24, "end": 0x23, "pageup": 0x21, "pagedown": 0x22,
}
for _i in range(1, 13):
  VK[f"f{_i}"] = 0x6F + _i  # F1 = 0x70, F2 = 0x71, ...

# These keys need the EXTENDEDKEY flag. Without it, Windows may treat them as numpad keys.
EXTENDED = {"delete", "insert", "left", "up", "right", "down", "home", "end", "pageup", "pagedown"}

KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002


def parse_hotkey(spec: str) -> list[tuple[int, bool]]:
  """Convert 'ctrl+shift+t' to [(vk, is_extended), ...].

  Raises ValueError if a key is unknown, so a bad combination is rejected early.
  """
  if not isinstance(spec, str) or not spec.strip():
    raise ValueError("empty key combination")

  keys: list[tuple[int, bool]] = []
  for token in spec.split("+"):
    t = token.strip().lower()
    if not t:
      # This also happens for "ctrl++", because the "+" key itself is not supported.
      raise ValueError("empty key in the combination (the '+' key is not supported)")
    if t in VK:
      keys.append((VK[t], t in EXTENDED))
    elif len(t) == 1 and t.isascii() and t.isalnum():
      # For letters and digits the virtual-key code is the ASCII code of the capital letter.
      keys.append((ord(t.upper()), False))
    else:
      raise ValueError(f"unknown key: {t}")
  return keys


def send_hotkey(keys: list[tuple[int, bool]]) -> None:
  """Press all keys in order, then release them in reverse order."""
  keybd_event = ctypes.windll.user32.keybd_event
  for vk, ext in keys:
    keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY if ext else 0, 0)
  for vk, ext in reversed(keys):
    keybd_event(vk, 0, (KEYEVENTF_EXTENDEDKEY if ext else 0) | KEYEVENTF_KEYUP, 0)


def validate_action(action) -> None:
  """Raise ValueError if the action is not valid. Accepts one action or a list of actions."""
  items = action if isinstance(action, list) else [action]
  if not items:
    raise ValueError("empty action")
  for a in items:
    if not isinstance(a, dict) or len(a) != 1:
      raise ValueError("an action must be a dict with one key: hotkey | run | delay")
    (kind, value), = a.items()
    if kind == "hotkey":
      parse_hotkey(value)
    elif kind == "run":
      if not isinstance(value, str) or not value.strip():
        raise ValueError("run: the command is empty")
    elif kind == "delay":
      # bool is a subclass of int in Python, so we reject it explicitly.
      if isinstance(value, bool) or not isinstance(value, (int, float)) or not (0 <= value <= 60):
        raise ValueError("delay: use a number of seconds from 0 to 60")
    else:
      raise ValueError(f"unknown action type: {kind}")


def execute_action(action) -> None:
  """Run one action or a list of actions one by one."""
  items = action if isinstance(action, list) else [action]
  for a in items:
    if "hotkey" in a:
      send_hotkey(parse_hotkey(a["hotkey"]))
    elif "run" in a:
      subprocess.Popen(a["run"], shell=True)  # we do not wait for the command to finish
    elif "delay" in a:
      time.sleep(float(a["delay"]))


class ActionWorker(threading.Thread):
  """Runs actions in a separate thread, so a "delay" never blocks the MQTT thread."""

  def __init__(self) -> None:
    super().__init__(name="actions", daemon=True)
    self._q: queue.Queue = queue.Queue()

  def submit(self, action) -> None:
    self._q.put(action)

  def run(self) -> None:
    while True:
      action = self._q.get()
      try:
        execute_action(action)
      except Exception:  # FR-D4: a failed action is logged and the daemon keeps working
        log.exception("action failed: %r", action)


#
# Config (FR-C1..C5, NFR-7)
#
def coerce_button(value) -> int:
  """Convert 16, '16' or '0x10' to the integer 16."""
  if isinstance(value, bool):
    raise ValueError("button")
  if isinstance(value, int):
    return value
  return int(str(value).strip(), 0)


class Config:
  """The only owner of the config file. All reads and writes go through this class."""

  def __init__(self, path: Path) -> None:
    self.path = path
    self.lock = threading.RLock()  # MQTT, IPC and timer threads all use the config
    self.data: dict = copy.deepcopy(DEFAULTS)

  # loading
  def load(self) -> None:
    """Read the file. A missing or broken file never stops the daemon (NFR-7)."""
    data = copy.deepcopy(DEFAULTS)
    try:
      raw = yaml.safe_load(self.path.read_text(encoding="utf-8"))
    except FileNotFoundError:
      log.info("config %s not found, using defaults", self.path)
      raw = None
    except (OSError, yaml.YAMLError, UnicodeDecodeError) as e:
      log.error("config is broken (%s), using defaults", e)
      try:
        # Keep the broken file, so the next save does not destroy the user's work.
        self.path.replace(self.path.with_suffix(".yaml.bad"))
      except OSError:
        pass
      raw = None

    if isinstance(raw, dict):
      for key in ("base_topic", "ipc_port", "learn_timeout"):
        if key in raw and raw[key] is not None:
          data[key] = raw[key]
      if isinstance(raw.get("mqtt"), dict):
        data["mqtt"].update(raw["mqtt"])
      data["apps"] = self._normalize_apps(raw.get("apps"))

    with self.lock:
      self.data = data

  @staticmethod
  def _normalize_apps(apps) -> list:
    """Keep only valid apps and bindings. Bad entries are skipped with a warning."""
    result = []
    if not isinstance(apps, list):
      return result
    for app in apps:
      if not isinstance(app, dict) or not isinstance(app.get("exe"), str):
        log.warning("skipping an invalid app entry: %r", app)
        continue
      bindings = []
      for b in app.get("bindings") or []:
        try:
          button = coerce_button(b["button"])
          # YAML 1.1 detail: PyYAML reads an unquoted key `on:` as the boolean True.
          on = b.get("on", b.get(True, "press"))
          if on not in ON_TYPES or "action" not in b:
            raise ValueError(on)
          bindings.append({"button": button, "on": on, "action": b["action"]})
        except (KeyError, ValueError, TypeError, AttributeError):
          log.warning("skipping an invalid binding in %s: %r", app["exe"], b)
      result.append({"exe": app["exe"], "bindings": bindings})
    return result

  # saving
  def _write(self, data: dict) -> None:
    """Atomic write: temp file in the same folder, then os.replace (FR-C2)."""
    self.path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix="config.", suffix=".tmp")
    try:
      with os.fdopen(fd, "w", encoding="utf-8") as f:
        yaml.safe_dump(data, f, sort_keys=False, allow_unicode=True)
        f.flush()
        os.fsync(f.fileno())  # make sure the bytes are on the disk before the rename
      os.replace(tmp, self.path)
    except BaseException:
      try:
        os.unlink(tmp)
      except OSError:
        pass
      raise

  # reading
  def get(self, key):
    with self.lock:
      return self.data[key]

  def snapshot_apps(self) -> list:
    """Return a copy, so the caller cannot change our data by accident."""
    with self.lock:
      return copy.deepcopy(self.data["apps"])

  def find_action(self, exe: str, button: int, on: str):
    """Find the action for (exe, button, on). The exe name is compared without case."""
    exe_l = exe.lower()
    with self.lock:
      for app in self.data["apps"]:
        if app["exe"].lower() != exe_l:
          continue
        for b in app["bindings"]:
          if b["button"] == button and b["on"] == on:
            return b["action"]
    return None

  # changes
  # We change a copy first. If the write fails, the old config stays unchanged.
  def add_binding(self, exe: str, button: int, on: str, action, force: bool) -> str | None:
    """Return 'added', 'replaced', or None (it already exists and force is not set)."""
    with self.lock:
      new = copy.deepcopy(self.data)
      app = next((a for a in new["apps"] if a["exe"].lower() == exe.lower()), None)
      if app is None:
        app = {"exe": exe, "bindings": []}
        new["apps"].append(app)

      existing = next((b for b in app["bindings"] if b["button"] == button and b["on"] == on), None)
      if existing is not None and not force:
        return None

      if existing is not None:
        existing["action"] = action
        result = "replaced"
      else:
        app["bindings"].append({"button": button, "on": on, "action": action})
        result = "added"

      self._write(new)
      self.data = new
      return result

  def remove_binding(self, exe: str, button: int, on: str | None) -> int:
    """Return how many bindings were removed. An app without bindings is removed too (FR-C5)."""
    with self.lock:
      new = copy.deepcopy(self.data)
      removed = 0
      for app in new["apps"]:
        if app["exe"].lower() != exe.lower():
          continue
        keep = []
        for b in app["bindings"]:
          if b["button"] == button and (on is None or b["on"] == on):
            removed += 1
          else:
            keep.append(b)
        app["bindings"] = keep
      new["apps"] = [a for a in new["apps"] if a["bindings"]]

      if removed:
        self._write(new)
        self.data = new
      return removed


#
# Win32: the active window (FR-W1..W5)
#
if IS_WIN:
  user32 = ctypes.WinDLL("user32", use_last_error=True)
  kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
  dwmapi = ctypes.WinDLL("dwmapi")

  # Callback types for the Windows API.
  WINEVENTPROC = ctypes.WINFUNCTYPE(None, wt.HANDLE, wt.DWORD, wt.HWND, wt.LONG, wt.LONG, wt.DWORD, wt.DWORD)
  WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
  CTRLHANDLER = ctypes.WINFUNCTYPE(wt.BOOL, wt.DWORD)

  # We set argtypes/restype for every function we use. Without this, ctypes can cut
  # 64-bit handles to 32 bits and the calls fail in strange ways.
  user32.GetForegroundWindow.restype = wt.HWND
  user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
  user32.GetWindowThreadProcessId.restype = wt.DWORD
  user32.SetWinEventHook.argtypes = [wt.DWORD, wt.DWORD, wt.HMODULE, WINEVENTPROC, wt.DWORD, wt.DWORD, wt.DWORD]
  user32.SetWinEventHook.restype = wt.HANDLE
  user32.UnhookWinEvent.argtypes = [wt.HANDLE]
  user32.GetMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT]
  user32.GetMessageW.restype = wt.BOOL
  user32.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
  user32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
  user32.PostThreadMessageW.argtypes = [wt.DWORD, wt.UINT, wt.WPARAM, wt.LPARAM]
  user32.EnumWindows.argtypes = [WNDENUMPROC, wt.LPARAM]
  user32.IsWindowVisible.argtypes = [wt.HWND]
  user32.GetWindowTextLengthW.argtypes = [wt.HWND]
  user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
  kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
  kernel32.OpenProcess.restype = wt.HANDLE
  kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
  kernel32.QueryFullProcessImageNameW.restype = wt.BOOL
  kernel32.CloseHandle.argtypes = [wt.HANDLE]
  kernel32.SetConsoleCtrlHandler.argtypes = [CTRLHANDLER, wt.BOOL]
  kernel32.GetCurrentThreadId.restype = wt.DWORD
  dwmapi.DwmGetWindowAttribute.argtypes = [wt.HWND, wt.DWORD, ctypes.c_void_p, wt.DWORD]

EVENT_SYSTEM_FOREGROUND = 0x0003
WINEVENT_OUTOFCONTEXT = 0x0000  # the callback runs in our thread, not inside the other process
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
DWMWA_CLOAKED = 14
WM_QUIT = 0x0012


def exe_of_hwnd(hwnd) -> str | None:
  """Return the exe file name of the window's process.

  Returns None if we have no access (for example, the window runs as administrator, FR-W5).
  """
  pid = wt.DWORD(0)
  user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
  if not pid.value:
    return None

  handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
  if not handle:
    return None
  try:
    buf = ctypes.create_unicode_buffer(1024)
    size = wt.DWORD(len(buf))
    if not kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
      return None
    return os.path.basename(buf.value)
  finally:
    kernel32.CloseHandle(handle)


def list_windows() -> list[dict]:
  """Return unique exe names of visible windows, with the title of the top window of each."""
  found: dict[str, dict] = {}

  def cb(hwnd, _lparam):
    if not user32.IsWindowVisible(hwnd):
      return True
    length = user32.GetWindowTextLengthW(hwnd)
    if length == 0:  # windows without a title are usually helper windows
      return True
    # Hidden UWP windows are also "visible" for Windows, so we skip the cloaked ones.
    cloaked = wt.DWORD(0)
    dwmapi.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
    if cloaked.value:
      return True
    exe = exe_of_hwnd(hwnd)
    if exe is None or exe.lower() in found:
      return True
    title = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, title, length + 1)
    found[exe.lower()] = {"exe": exe, "title": title.value}
    return True  # True = continue the enumeration

  proc = WNDENUMPROC(cb)
  user32.EnumWindows(proc, 0)
  return sorted(found.values(), key=lambda w: w["exe"].lower())


class ActiveWindow:
  """Keeps the exe name of the active window in a cache.

  The cache is updated by Windows events (no polling). Button presses just read the cache.
  """

  def __init__(self) -> None:
    self.exe: str | None = None
    self._timer: threading.Timer | None = None
    self._lock = threading.Lock()
    self._hook = None
    self._proc = None  # we keep a reference, otherwise the garbage collector destroys the callback

  def install(self) -> None:
    self._proc = WINEVENTPROC(self._on_event)
    self._hook = user32.SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, None, self._proc, 0, 0, WINEVENT_OUTOFCONTEXT
    )
    if not self._hook:
      raise OSError("SetWinEventHook failed")
    self._refresh()  # read the current window once at start

  def uninstall(self) -> None:
    if self._hook:
      user32.UnhookWinEvent(self._hook)
      self._hook = None

  def _on_event(self, *_args) -> None:
    # Alt+Tab creates a series of events. Every new event restarts the timer,
    # so we read the window only once, after the series is finished (debounce).
    with self._lock:
      if self._timer is not None:
        self._timer.cancel()
      self._timer = threading.Timer(WINDOW_DEBOUNCE_S, self._refresh)
      self._timer.daemon = True
      self._timer.start()

  def _refresh(self) -> None:
    hwnd = user32.GetForegroundWindow()
    if not hwnd:  # a short state between two windows: keep the previous value
      return
    self.exe = exe_of_hwnd(hwnd)
    log.debug("active window: %s", self.exe)


#
# MQTT payload (spec 6.1) - all parsing is in this one function
#
def parse_button_event(topic: str, payload: bytes) -> tuple[int, str] | None:
  """Return (button_id, event_type), or None if the payload is not valid.

  Payload format published by bridge.c on the ESP32:
    {"event":"pressed","button_id":16,"name":"A","timestamp_ms":..,
     "duration_ms":..,"click_count":..}
  """
  try:
    d = json.loads(payload.decode("utf-8"))
    if not isinstance(d, dict):
      return None
    on = EVENT_MAP.get(d.get("event"))
    if on is None:
      return None
    button_id = d.get("button_id")
    if button_id is None:
      # Fallback: unknown buttons have the name "0xNN", so we can read the id from it.
      name = d.get("name") or topic.split("/")[-2]
      button_id = int(str(name), 0)
    return coerce_button(button_id), on
  except (ValueError, TypeError, IndexError):
    return None


#
# Learn mode (FR-L1..L5)
#
class Learn:
  """State of the learn session. Only one session can be active at a time (FR-L5)."""

  def __init__(self) -> None:
    self._lock = threading.Lock()
    self._q: queue.Queue | None = None

  @property
  def active(self) -> bool:
    return self._q is not None

  def start(self) -> queue.Queue | None:
    """Start a session. Returns None if another session is already running."""
    with self._lock:
      if self._q is not None:
        return None
      self._q = queue.Queue(maxsize=1)
      return self._q

  def stop(self) -> None:
    with self._lock:
      self._q = None

  def offer(self, button: int) -> bool:
    """Give the pressed button to the waiting session. Only the first one is kept."""
    with self._lock:
      if self._q is None:
        return False
      try:
        self._q.put_nowait(button)
      except queue.Full:
        pass
      return True


#
# The daemon itself
#
class Deckd:
  def __init__(self, config: Config) -> None:
    self.cfg = config
    self.window = ActiveWindow()
    self.learn = Learn()
    self.worker = ActionWorker()
    self.mqtt_connected = False
    self.client: mqtt.Client | None = None
    self.server: socketserver.ThreadingTCPServer | None = None

    # Buttons whose "tail" events (release, click, long) must be ignored after learn mode.
    self._suppressed: set[int] = set()
    self._sup_lock = threading.Lock()

  # MQTT
  def start_mqtt(self) -> None:
    m = self.cfg.get("mqtt")
    # The client id must be different from the ESP32 id ("stm32-bridge"),
    # because the broker disconnects the old client when two use the same id.
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="deckd")
    if m.get("username"):
      client.username_pw_set(m["username"], m.get("password"))
    client.reconnect_delay_set(min_delay=1, max_delay=10)  # NFR-3: automatic reconnect
    client.on_connect = self._on_connect
    client.on_disconnect = self._on_disconnect
    client.on_message = self._on_message
    self.client = client
    # connect_async: the daemon starts even if the broker is not ready yet.
    client.connect_async(m["host"], int(m["port"]), keepalive=30)
    client.loop_start()  # the network loop runs in its own thread

  def _topic(self, suffix: str) -> str:
    return f"{self.cfg.get('base_topic')}/{suffix}"

  def _on_connect(self, client, _userdata, _flags, reason_code, _props) -> None:
    if reason_code != 0:
      log.error("MQTT: the broker refused the connection: %s", reason_code)
      return
    self.mqtt_connected = True
    log.info("MQTT connected")
    # Subscribe on every (re)connect, because a clean session forgets old subscriptions.
    client.subscribe(self._topic("button/+/event"), qos=1)

  def _on_disconnect(self, _client, _userdata, _flags, reason_code, _props) -> None:
    self.mqtt_connected = False
    log.warning("MQTT disconnected (%s), reconnecting...", reason_code)

  def _on_message(self, _client, _userdata, msg) -> None:
    try:
      parsed = parse_button_event(msg.topic, msg.payload)
      if parsed is None:
        log.warning("invalid payload in %s: %r", msg.topic, msg.payload[:100])
        return
      self.handle_button(*parsed)
    except Exception:  # NFR-6: bad data must never crash the daemon
      log.exception("failed to handle an MQTT message")

  def publish_display(self, text: str) -> None:
    """Send a short hint for the OLED. It works only if the ESP32 subscribes to this topic."""
    if self.client is not None and self.mqtt_connected:
      self.client.publish(self._topic("cmd/display"), text.encode("utf-8"), qos=0)

  # button events
  def handle_button(self, button: int, on: str) -> None:
    log.debug("button 0x%02X %s", button, on)

    # The "tail" of the press that finished learn mode: release, click and long
    # events must not run any action (FR-L3). A new press ends the suppression.
    with self._sup_lock:
      if button in self._suppressed:
        if on == "press":
          self._suppressed.discard(button)
        else:
          if on == "release":
            # A click event can still come after the release, so we wait a bit.
            t = threading.Timer(SUPPRESS_AFTER_RELEASE_S, self._unsuppress, args=(button,))
            t.daemon = True
            t.start()
          return

    # Learn mode: take the first "press" and ignore all other events.
    # No actions run while learn mode is active (FR-L2, FR-L3).
    if self.learn.active:
      if on == "press" and self.learn.offer(button):
        with self._sup_lock:
          self._suppressed.add(button)
      return

    exe = self.window.exe  # read from the cache, no system call here (FR-W4)
    if exe is None:  # unknown window, for example an administrator window (FR-W5)
      return

    # Run the action only if (exe, button, event type) match a binding (FR-D1).
    action = self.cfg.find_action(exe, button, on)
    if action is not None:
      self.worker.submit(action)

  def _unsuppress(self, button: int) -> None:
    with self._sup_lock:
      self._suppressed.discard(button)

  # IPC
  def handle_request(self, req: dict) -> dict:
    """Route an IPC request to its handler."""
    cmd = req.get("cmd")
    handler = {
      "windows": self._cmd_windows,
      "learn": self._cmd_learn,
      "check_keys": self._cmd_check_keys,
      "bind_add": self._cmd_bind_add,
      "bind_remove": self._cmd_bind_remove,
      "bind_list": self._cmd_bind_list,
      "reload": self._cmd_reload,
      "status": self._cmd_status,
    }.get(cmd)
    if handler is None:
      return {"ok": False, "error": "unknown_cmd"}
    return handler(req)

  def _cmd_windows(self, _req) -> dict:
    return {"ok": True, "windows": list_windows()}

  def _cmd_learn(self, _req) -> dict:
    """Wait for the next button press and return its id."""
    q = self.learn.start()
    if q is None:
      return {"ok": False, "error": "busy"}
    try:
      self.publish_display("Press a button")  # optional hint on the OLED (FR-L6)
      try:
        button = q.get(timeout=float(self.cfg.get("learn_timeout")))
      except queue.Empty:
        return {"ok": False, "error": "timeout"}
      return {"ok": True, "button": button}
    finally:
      self.learn.stop()  # always leave learn mode, even after an error

  def _cmd_check_keys(self, req) -> dict:
    """Check a key combination, so deckctl can fail early (before waiting for a button)."""
    try:
      parse_hotkey(req.get("keys"))
    except ValueError as e:
      return {"ok": False, "error": "bad_key", "detail": str(e)}
    return {"ok": True}

  def _cmd_bind_add(self, req) -> dict:
    try:
      exe = str(req["exe"]).strip()
      button = coerce_button(req["button"])
      on = req.get("on", "press")
      action = req["action"]
      if not exe or on not in ON_TYPES:
        raise ValueError
    except (KeyError, ValueError, TypeError):
      return {"ok": False, "error": "bad_request"}

    try:
      validate_action(action)  # FR-D6: on error the config is not changed
    except ValueError as e:
      return {"ok": False, "error": "bad_key", "detail": str(e)}

    try:
      result = self.cfg.add_binding(exe, button, on, action, bool(req.get("force")))
    except OSError as e:
      log.error("could not write the config: %s", e)
      return {"ok": False, "error": "write_failed", "detail": str(e)}

    if result is None:
      return {"ok": False, "error": "exists"}
    return {"ok": True, "replaced": result == "replaced"}

  def _cmd_bind_remove(self, req) -> dict:
    try:
      exe = str(req["exe"]).strip()
      button = coerce_button(req["button"])
      on = req.get("on")  # None means "all event types of this button"
      if on is not None and on not in ON_TYPES:
        raise ValueError
    except (KeyError, ValueError, TypeError):
      return {"ok": False, "error": "bad_request"}

    try:
      removed = self.cfg.remove_binding(exe, button, on)
    except OSError as e:
      return {"ok": False, "error": "write_failed", "detail": str(e)}
    if removed == 0:
      return {"ok": False, "error": "not_found"}
    return {"ok": True, "removed": removed}

  def _cmd_bind_list(self, _req) -> dict:
    return {"ok": True, "apps": self.cfg.snapshot_apps()}

  def _cmd_reload(self, _req) -> dict:
    self.cfg.load()
    return {"ok": True}

  def _cmd_status(self, _req) -> dict:
    return {"ok": True, "active": self.window.exe, "mqtt": self.mqtt_connected}

  # startup
  def start_ipc(self) -> None:
    """Start the local TCP server. One JSON request per line, one JSON answer per line."""
    deckd = self

    class Handler(socketserver.StreamRequestHandler):
      def handle(self) -> None:
        line = self.rfile.readline(65536)  # limit the size of one request
        if not line:
          return
        try:
          req = json.loads(line.decode("utf-8"))
          if not isinstance(req, dict):
            raise ValueError
        except ValueError:  # this includes JSONDecodeError and UnicodeDecodeError
          resp = {"ok": False, "error": "bad_request"}
        else:
          try:
            resp = deckd.handle_request(req)
          except Exception:
            log.exception("IPC request failed")
            resp = {"ok": False, "error": "internal"}
        try:
          self.wfile.write((json.dumps(resp, ensure_ascii=False) + "\n").encode("utf-8"))
        except OSError:
          pass  # the client is gone (for example, Ctrl+C during learn mode)

    class Server(socketserver.ThreadingTCPServer):
      # On Windows, SO_REUSEADDR would allow two daemons to listen on the same port.
      allow_reuse_address = False
      daemon_threads = True  # one thread per client, so learn mode does not block other requests

    port = int(self.cfg.get("ipc_port"))
    self.server = Server(("127.0.0.1", port), Handler)  # NFR-5: localhost only
    threading.Thread(target=self.server.serve_forever, name="ipc", daemon=True).start()
    log.info("IPC listens on 127.0.0.1:%d", port)

  def run(self) -> int:
    self.worker.start()
    self.start_ipc()
    self.window.install()
    self.start_mqtt()

    main_tid = kernel32.GetCurrentThreadId()

    def on_ctrl(_event) -> bool:
      # GetMessageW blocks the main thread, so Python would not see Ctrl+C.
      # Windows calls this handler in another thread, and we wake the main thread.
      user32.PostThreadMessageW(main_tid, WM_QUIT, 0, 0)
      return True

    ctrl = CTRLHANDLER(on_ctrl)  # keep the reference while we run
    kernel32.SetConsoleCtrlHandler(ctrl, True)

    log.info("deckd is running. Press Ctrl+C to stop.")
    msg = wt.MSG()
    # SetWinEventHook needs a message loop in the same thread to deliver its events.
    while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
      user32.TranslateMessage(ctypes.byref(msg))
      user32.DispatchMessageW(ctypes.byref(msg))

    log.info("stopping...")
    self.window.uninstall()
    if self.client is not None:
      self.client.loop_stop()
      self.client.disconnect()
    if self.server is not None:
      self.server.shutdown()
    return 0


def main() -> int:
  logging.basicConfig(
    level=logging.DEBUG if "-v" in sys.argv else logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
  )
  if not IS_WIN:
    print("deckd works only on Windows.", file=sys.stderr)
    return 1

  config = Config(default_config_path())
  config.load()
  try:
    return Deckd(config).run()
  except OSError as e:
    log.error("could not start: %s (is the IPC port busy? is deckd already running?)", e)
    return 1


if __name__ == "__main__":
  sys.exit(main())
