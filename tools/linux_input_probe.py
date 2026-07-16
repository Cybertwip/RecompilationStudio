#!/usr/bin/env python3
"""Stream the runtime's host-input -> SIO state over the TCP debug protocol."""

import json
import socket
import sys
import time


def request(command):
    """Send one command using the server's one-command-per-connection wire protocol."""
    with socket.create_connection(("127.0.0.1", 4370), timeout=5.0) as sock:
        sock.settimeout(5.0)
        with sock.makefile("rwb", buffering=0) as stream:
            stream.write((json.dumps(command, separators=(",", ":")) + "\n").encode())
            line = stream.readline()
    if not line.strip():
        raise RuntimeError("runtime returned an empty debug response")
    try:
        response = json.loads(line)
    except json.JSONDecodeError as error:
        raw = line.decode("utf-8", "backslashreplace").rstrip()
        raise RuntimeError("invalid debug response %r: %s" % (raw, error)) from error
    if not response.get("ok"):
        raise RuntimeError(response.get("error", "debug command failed"))
    return response


def snapshot():
    frontend = request({"id": 1, "cmd": "frontend_input_state"})["state"]
    pad = request({"id": 2, "cmd": "pad_status"})
    return {
        "video_driver": frontend.get("video_driver"),
        "joysticks": frontend.get("joysticks"),
        "controller_mappings": frontend.get("external_controller_mappings"),
        "p1_kind": frontend.get("p1_kind"),
        "p1_sdl_attached": frontend.get("p1_sdl_attached"),
        "keyboard_pad": frontend.get("keyboard_pad"),
        "physical_pad": frontend.get("physical_pad"),
        "routed_pad": frontend.get("routed_pad"),
        "sdl_buttons": frontend.get("sdl_buttons"),
        "sdl_axes": [0 if abs(value) < 4096 else value
                     for value in frontend.get("sdl_axes", [])],
        "sio_pad": frontend.get("sio_pad"),
        "sio_connected": frontend.get("sio_connected"),
        "sio_analog": frontend.get("sio_analog"),
        "pad_status": pad.get("slot0"),
        "event_total": frontend.get("total"),
        "last_event": frontend.get("entries", [])[-1:] or None,
    }


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    request({"id": 0, "cmd": "pad_status"})
    print("Connected. During the next %.0f seconds, press and release:" % duration)
    print("  keyboard: Enter, X, and an arrow key")
    print("  gamepad: Start, A, and a D-pad direction")
    previous = None
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        current = snapshot()
        if current != previous:
            print(json.dumps(current, sort_keys=True))
            previous = current
        time.sleep(0.10)
    print("Sending a 12-frame PSX Start pulse through the debug input path.")
    print("Watch whether the game reacts even if the physical inputs did not.")
    request({"id": 3, "cmd": "press", "buttons": 0xFFF7, "frames": 12})
    time.sleep(0.35)
    print(json.dumps(snapshot(), sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print("input probe failed: %s" % error, file=sys.stderr)
        raise SystemExit(1)
