#!/usr/bin/env python3
"""
deckctl - command line tool for deckd (bind Stream Deck buttons to applications).

Usage:
  deckctl windows
  deckctl bind add [--exe Code.exe] [--keys ctrl+v] [--on press] [--force]
  deckctl bind list
  deckctl bind remove --exe Code.exe --button 0x10 [--on press]
  deckctl status

deckctl never changes the config file itself. It sends requests to deckd over IPC,
and deckd is the only program that writes the config.

IPC port: --port, or the DECK_IPC_PORT variable, or ipc_port from config.yaml, or 47800.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
from pathlib import Path

DEFAULT_PORT = 47800
ON_TYPES = ("press", "release", "click", "long")

# Error codes from deckd -> messages for the user.
ERRORS = {
  "timeout": "No button was pressed in time. The config was not changed.",
  "busy": "Another learn session is already running. Please wait until it ends.",
  "exists": "This binding already exists.",
  "not_found": "Binding not found.",
  "bad_request": "deckd did not understand the request (invalid parameters).",
  "unknown_cmd": "deckd does not know this command. The deckd and deckctl versions may be different.",
  "write_failed": "deckd could not write the config file.",
  "internal": "Internal error in deckd. See its log for details.",
}


class CtlError(Exception):
  """An error that we show to the user as a short message, without a traceback."""


#
# IPC
#
def resolve_port(cli_port: int | None) -> int:
  """Choose the IPC port: command line, environment, config file, default (in this order)."""
  if cli_port:
    return cli_port
  if os.environ.get("DECK_IPC_PORT"):
    return int(os.environ["DECK_IPC_PORT"])
  try:
    # We only READ the port here. We never change the config.
    import yaml

    path = Path(os.environ.get("DECK_CONFIG") or Path(os.environ.get("APPDATA", Path.home())) / "deck" / "config.yaml")
    raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    if isinstance(raw, dict) and raw.get("ipc_port"):
      return int(raw["ipc_port"])
  except Exception:
    pass  # no config or no PyYAML: use the default port
  return DEFAULT_PORT


def call(port: int, request: dict, timeout: float = 10.0) -> dict:
  """Send one JSON request and return the JSON answer. Raises CtlError on connection problems."""
  try:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
      s.settimeout(timeout)
      s.sendall((json.dumps(request) + "\n").encode("utf-8"))
      # The answer is one line, so we read until the newline.
      data = b""
      while not data.endswith(b"\n"):
        chunk = s.recv(4096)
        if not chunk:
          break
        data += chunk
  except ConnectionRefusedError:
    raise CtlError(f"deckd is not running (no answer on 127.0.0.1:{port}). Start it with: python deckd.py")
  except socket.timeout:
    raise CtlError("deckd did not answer in time.")
  except OSError as e:
    raise CtlError(f"could not talk to deckd: {e}")

  try:
    return json.loads(data.decode("utf-8"))
  except ValueError:
    raise CtlError("deckd sent an invalid answer.")


def explain(resp: dict) -> str:
  """Turn an error answer from deckd into a clear message."""
  err = resp.get("error", "unknown")
  if err == "bad_key":
    return f"Unknown or invalid key combination: {resp.get('detail', '')}"
  text = ERRORS.get(err, f"error: {err}")
  if resp.get("detail"):
    text += f" ({resp['detail']})"
  return text


def parse_button(value: str) -> int:
  """Read a button id written as 16 or 0x10."""
  try:
    return int(value, 0)
  except ValueError:
    raise CtlError(f"invalid button id: {value!r} (examples: 16 or 0x10)")


#
# Commands
#
def cmd_windows(args) -> int:
  resp = call(args.port, {"cmd": "windows"})
  if not resp.get("ok"):
    raise CtlError(explain(resp))
  for w in resp["windows"]:
    print(f"{w['exe']:<30} {w['title']}")
  return 0


def choose_exe(port: int) -> str:
  """Show a numbered list of running applications and let the user choose one."""
  resp = call(port, {"cmd": "windows"})
  if not resp.get("ok"):
    raise CtlError(explain(resp))
  windows = resp["windows"]

  for i, w in enumerate(windows, 1):
    print(f"{i:>3}) {w['exe']:<30} {w['title'][:50]}")
  answer = input("Enter a number from the list or an exe name: ").strip()

  if answer.isdigit() and 1 <= int(answer) <= len(windows):
    return windows[int(answer) - 1]["exe"]
  if not answer:
    raise CtlError("no application selected.")
  return answer  # the user typed an exe name directly


def cmd_bind_add(args) -> int:
  exe = args.exe or choose_exe(args.port)
  keys = args.keys or input("Key combination (for example, ctrl+v): ").strip()

  # Check the combination BEFORE we wait for a button, so the user does not press it for nothing.
  check = call(args.port, {"cmd": "check_keys", "keys": keys})
  if not check.get("ok"):
    raise CtlError(explain(check))

  print("Press a button on the deck...")
  try:
    # The socket timeout is longer than learn_timeout in deckd (15 s by default).
    learn = call(args.port, {"cmd": "learn"}, timeout=60.0)
  except KeyboardInterrupt:
    raise CtlError("cancelled.")
  if not learn.get("ok"):
    raise CtlError(explain(learn))
  button = learn["button"]

  request = {
    "cmd": "bind_add",
    "exe": exe,
    "button": button,
    "on": args.on,
    "action": {"hotkey": keys},
    "force": args.force,
  }
  resp = call(args.port, request)

  # The binding exists: ask the user before we replace it (FR-C3).
  if not resp.get("ok") and resp.get("error") == "exists":
    answer = input(f"A binding for {exe} + button 0x{button:02X} ({args.on}) already exists. Replace it? [y/N]: ")
    if answer.strip().lower() not in ("y", "yes"):
      raise CtlError("replacement cancelled, the config was not changed.")
    request["force"] = True
    resp = call(args.port, request)

  if not resp.get("ok"):
    raise CtlError(explain(resp))

  print(f"{exe} + {keys} -> button 0x{button:02X} ({args.on})" + (" [replaced]" if resp.get("replaced") else ""))
  return 0


def cmd_bind_list(args) -> int:
  resp = call(args.port, {"cmd": "bind_list"})
  if not resp.get("ok"):
    raise CtlError(explain(resp))
  apps = resp["apps"]
  if not apps:
    print("No bindings.")
    return 0
  for app in apps:
    print(app["exe"])
    for b in app["bindings"]:
      action = b["action"]
      if isinstance(action, dict):
        kind, value = next(iter(action.items()))
      else:  # a list of actions (possible when the config is edited by hand)
        kind, value = "sequence", f"{len(action)} steps"
      print(f"    button 0x{b['button']:02X} ({b['on']:<7}) -> {kind}: {value}")
  return 0


def cmd_bind_remove(args) -> int:
  request = {"cmd": "bind_remove", "exe": args.exe, "button": parse_button(args.button)}
  if args.on:
    request["on"] = args.on
  resp = call(args.port, request)
  if not resp.get("ok"):
    raise CtlError(explain(resp))
  print(f"Removed bindings: {resp['removed']}")
  return 0


def cmd_status(args) -> int:
  resp = call(args.port, {"cmd": "status"})
  if not resp.get("ok"):
    raise CtlError(explain(resp))
  print(f"Active exe: {resp['active'] or 'unknown'}")
  mqtt_text = "connected" if resp["mqtt"] else "not connected"
  print(f"MQTT:       {mqtt_text}")
  return 0


#
# Command line parser
#
def build_parser() -> argparse.ArgumentParser:
  p = argparse.ArgumentParser(prog="deckctl", description="Manage DIY Stream Deck bindings")
  p.add_argument("--port", type=int, help=f"deckd IPC port (default {DEFAULT_PORT})")
  sub = p.add_subparsers(dest="command", required=True)

  sub.add_parser("windows", help="list applications (exe) that have visible windows").set_defaults(func=cmd_windows)
  sub.add_parser("status", help="show the active exe and the MQTT state").set_defaults(func=cmd_status)

  bind = sub.add_parser("bind", help="manage bindings")
  bsub = bind.add_subparsers(dest="bind_command", required=True)

  add = bsub.add_parser("add", help="create or replace a binding (uses learn mode)")
  add.add_argument("--exe", help="application exe, for example Code.exe")
  add.add_argument("--keys", help="key combination, for example ctrl+shift+t")
  add.add_argument("--on", choices=ON_TYPES, default="press", help="event type (default: press)")
  add.add_argument("--force", action="store_true", help="replace an existing binding without asking")
  add.set_defaults(func=cmd_bind_add)

  bsub.add_parser("list", help="show all bindings").set_defaults(func=cmd_bind_list)

  rm = bsub.add_parser("remove", help="remove a binding")
  rm.add_argument("--exe", required=True)
  rm.add_argument("--button", required=True, help="button id: 16 or 0x10")
  rm.add_argument("--on", choices=ON_TYPES, help="if not set, all event types of this button are removed")
  rm.set_defaults(func=cmd_bind_remove)
  return p


def main() -> int:
  args = build_parser().parse_args()
  args.port = resolve_port(args.port)
  try:
    return args.func(args)
  except CtlError as e:
    print(f"Error: {e}", file=sys.stderr)
    return 1
  except (KeyboardInterrupt, EOFError):
    print("\nCancelled.", file=sys.stderr)
    return 1


if __name__ == "__main__":
  sys.exit(main())
