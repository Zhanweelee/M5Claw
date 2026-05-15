#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M5Claw debug console — serial monitor with command palette"""
import sys
import os
import re
import time
import json
import subprocess
import threading
import queue

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("pyserial required. Install: pip install pyserial")
    sys.exit(1)

CACHE_FILE = ".flash_cache.json"
BAUD = 115200

# ── commands that need an argument ──
COMMANDS = [
    # (name,       needs_arg,  arg_hint)
    ("help",          False,  ""),
    ("show_config",   False,  ""),
    ("list_providers",False,  ""),
    ("set_provider",  True,   "mimo / deepseek / openai / anthropic / custom"),
    ("set_llm_key",   True,   "sk-..."),
    ("set_llm_model", True,   "model-name"),
    ("set_wifi",      True,   "SSID password"),
    ("set_wifi2",     True,   "SSID password"),
    ("set_city",      True,   "Beijing"),
    ("set_wechat",    True,   "token host"),
    ("set_tts_provider", True, "siliconflow"),
    ("set_tts_key",   True,   "sk-..."),
    ("set_tts_model", True,   "model-name"),
    ("set_tts_voice", True,   "voice-name"),
    ("reset_config",  False,  ""),
    ("reboot",        False,  ""),
]

# ── serial port scanner ──
def scan_ports():
    blocked = {"debug-console", "Bluetooth-Incoming-Port"}
    ports = []
    for p in serial.tools.list_ports.comports():
        name = os.path.basename(p.device) if hasattr(os, "basename") else p.device
        # Filter out macOS pseudo-devices by device path
        if any(b in p.device for b in blocked):
            continue
        # Filter genuine Bluetooth adapters (not ESP32 descriptors)
        if "bluetooth" in p.description.lower() and p.vid is None:
            continue
        ports.append(p.device)
    return sorted(ports)


def load_cached_port(project_dir):
    path = os.path.join(project_dir, CACHE_FILE)
    if os.path.exists(path):
        try:
            with open(path, "r") as f:
                data = json.load(f)
                return data.get("_last_port", "")
        except Exception:
            pass
    return ""


def save_cached_port(project_dir, port):
    path = os.path.join(project_dir, CACHE_FILE)
    cache = {}
    if os.path.exists(path):
        try:
            with open(path, "r") as f:
                cache = json.load(f)
        except Exception:
            pass
    cache["_last_port"] = port
    with open(path, "w") as f:
        json.dump(cache, f, indent=2, ensure_ascii=False)


def select_port(project_dir):
    ports = scan_ports()
    if not ports:
        print("No serial ports found. Connect the device and try again.")
        sys.exit(1)

    cached = load_cached_port(project_dir)
    if len(ports) == 1:
        print(f"Auto-selected: {ports[0]}")
        return ports[0]

    print()
    print("Available ports:")
    for i, p in enumerate(ports, 1):
        tag = " (cached)" if p == cached else ""
        print(f"  [{i}] {p}{tag}")
    print("  [0] Exit")

    default_idx = 1
    if cached in ports:
        default_idx = ports.index(cached) + 1

    while True:
        try:
            sel = input(f"Select port (1-{len(ports)}) [{default_idx}]: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("Cancelled")
            sys.exit(0)
        if sel == "0":
            sys.exit(0)
        if sel == "":
            return ports[default_idx - 1]
        try:
            idx = int(sel)
            if 1 <= idx <= len(ports):
                return ports[idx - 1]
        except ValueError:
            pass
        print("Invalid input")


# ── curses TUI ──
def run_tui(port):
    import curses
    import curses.textpad

    ser = serial.Serial(port, BAUD, timeout=0.05)
    ser.reset_input_buffer()

    log_queue = queue.Queue()
    running = True

    def reader_thread():
        buf = b""
        while running:
            try:
                data = ser.read(512)
                if data:
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        decoded = line.decode("utf-8", errors="replace").rstrip()
                        if decoded:
                            log_queue.put(decoded)
                else:
                    time.sleep(0.01)
            except Exception:
                break

    thread = threading.Thread(target=reader_thread, daemon=True)
    thread.start()

    stdscr = curses.initscr()
    curses.noecho()
    curses.cbreak()
    curses.curs_set(1)
    stdscr.keypad(True)
    stdscr.nodelay(True)

    # Colors
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_CYAN,    -1)  # header
    curses.init_pair(2, curses.COLOR_GREEN,   -1)  # success / sent
    curses.init_pair(3, curses.COLOR_YELLOW,  -1)  # warning / system
    curses.init_pair(4, curses.COLOR_RED,     -1)  # error
    curses.init_pair(5, curses.COLOR_WHITE,   -1)  # normal log
    curses.init_pair(6, curses.COLOR_BLACK,   curses.COLOR_CYAN)  # selected command

    MAX_LOG = 500
    logs = []
    cmd_selected = 0
    input_buf = ""
    input_mode = False
    input_prompt = ""
    input_cursor = 0
    status_msg = f"Connected to {port} @ {BAUD} | Ctrl+C or F10 to quit"
    scroll_offset = 0

    def add_log(text):
        ts = time.strftime("%H:%M:%S")
        # Color-code: errors in red, LLM lines in cyan, agent in green
        color = 5
        if "[LLM]" in text or "[TTS]" in text:
            color = 1
        elif "[AGENT]" in text or "[CHAT]" in text:
            color = 2
        elif "[WIFI]" in text or "[BOOT]" in text or "[CONFIG]" in text:
            color = 3
        elif "error" in text.lower() or "fail" in text.lower() or "Error" in text:
            color = 4
        logs.append((ts, text, color))
        if len(logs) > MAX_LOG:
            logs.pop(0)

    def send_cmd(cmd_line):
        ser.write((cmd_line + "\r\n").encode())
        add_log(f">>> {cmd_line}")

    MOVE_KEYS = {curses.KEY_UP: -1, curses.KEY_DOWN: 1,
                 259: -1, 258: 1,  # ncurses alt codes
                 ord('k'): -1, ord('j'): 1}

    try:
        while True:
            # ── process input ──
            try:
                ch = stdscr.getch()
            except Exception:
                ch = -1

            if ch == curses.KEY_F10 or ch == 27:  # ESC or F10
                break

            if input_mode:
                if ch == 27:  # ESC cancel input
                    input_mode = False
                    input_buf = ""
                    input_cursor = 0
                elif ch == 10 or ch == 13:  # Enter
                    if input_buf.strip():
                        send_cmd(input_buf.strip())
                    input_mode = False
                    input_buf = ""
                    input_cursor = 0
                elif ch == curses.KEY_BACKSPACE or ch == 127 or ch == 8:
                    if input_cursor > 0:
                        input_buf = input_buf[:input_cursor-1] + input_buf[input_cursor:]
                        input_cursor -= 1
                elif ch == curses.KEY_LEFT:
                    if input_cursor > 0:
                        input_cursor -= 1
                elif ch == curses.KEY_RIGHT:
                    if input_cursor < len(input_buf):
                        input_cursor += 1
                elif 32 <= ch <= 126:
                    input_buf = input_buf[:input_cursor] + chr(ch) + input_buf[input_cursor:]
                    input_cursor += 1
            else:
                if ch in MOVE_KEYS:
                    cmd_selected = (cmd_selected + MOVE_KEYS[ch]) % len(COMMANDS)
                elif ch == 10 or ch == 13:  # Enter on command
                    name, needs_arg, hint = COMMANDS[cmd_selected]
                    if needs_arg:
                        input_mode = True
                        input_prompt = f"{name} [{hint}]"
                        input_buf = name + " "
                        input_cursor = len(input_buf)
                    else:
                        send_cmd(name)
                elif ch == ord(':'):
                    input_mode = True
                    input_prompt = "custom"
                    input_buf = ""
                    input_cursor = 0
                elif ch == ord('r'):
                    # Quick reboot
                    send_cmd("reboot")
                elif ch == ord('q'):
                    break

            # ── drain log queue ──
            while True:
                try:
                    line = log_queue.get_nowait()
                    add_log(line)
                except queue.Empty:
                    break

            # ── render ──
            h, w = stdscr.getmaxyx()
            mid = max(30, min(50, w // 3))

            stdscr.erase()

            # Header
            header = " M5Claw Debug Console "
            stdscr.attron(curses.color_pair(1) | curses.A_BOLD)
            stdscr.addstr(0, 0, header[:w - 1].ljust(w - 1))
            stdscr.attroff(curses.color_pair(1) | curses.A_BOLD)

            # Left panel: commands
            stdscr.attron(curses.color_pair(1))
            stdscr.addstr(1, 1, " Commands ".ljust(mid - 2))
            stdscr.attroff(curses.color_pair(1))

            left_body = mid - 2
            visible_cmds = min(len(COMMANDS), h - 5)

            for i, (name, needs_arg, hint) in enumerate(COMMANDS):
                if i >= h - 5:
                    break
                y = 2 + i
                label = f" {name}"
                if needs_arg:
                    label += " ..."
                if i == cmd_selected:
                    stdscr.attron(curses.color_pair(6) | curses.A_BOLD)
                    stdscr.addstr(y, 1, label.ljust(left_body)[:left_body])
                    stdscr.attroff(curses.color_pair(6) | curses.A_BOLD)
                else:
                    stdscr.addstr(y, 1, label[:left_body])

            # Vertical divider
            for y in range(1, h - 2):
                try:
                    stdscr.addstr(y, mid, curses.ACS_VLINE)
                except Exception:
                    stdscr.addstr(y, mid, "|")

            # Right panel: logs
            stdscr.attron(curses.color_pair(1))
            stdscr.addstr(1, mid + 1, " Logs ".ljust(w - mid - 1)[:w - mid - 2])
            stdscr.attroff(curses.color_pair(1))

            log_area_h = h - 5
            start = max(0, len(logs) - log_area_h - scroll_offset)
            end = len(logs)
            while end - start > log_area_h:
                start += 1

            for i, idx in enumerate(range(start, end)):
                y = 2 + i
                if y >= h - 3:
                    break
                ts, text, color = logs[idx]
                line = f"{ts} {text}"
                stdscr.attron(curses.color_pair(color))
                stdscr.addstr(y, mid + 1, line[:w - mid - 2])
                stdscr.attroff(curses.color_pair(color))

            # Input bar
            if input_mode:
                prefix = f" {input_prompt}: "
                visible = input_buf[max(0, input_cursor - (w - len(prefix) - 3)):input_cursor + (w - len(prefix) - 10)]
                cursor_x = mid + 1 + len(prefix) + (input_cursor - max(0, input_cursor - (w - len(prefix) - 3)))
                if cursor_x < mid + 1:
                    cursor_x = mid + 1 + len(prefix)
                if cursor_x >= w - 1:
                    cursor_x = w - 2
                stdscr.attron(curses.color_pair(3))
                stdscr.addstr(h - 2, mid + 1, (prefix + visible)[:w - mid - 2])
                stdscr.attroff(curses.color_pair(3))
                try:
                    stdscr.move(h - 2, min(cursor_x, w - 2))
                except Exception:
                    pass
            else:
                # Hint line
                hint = " Enter:send  j/k:nav  ::raw  r:reboot  q:quit"
                stdscr.attron(curses.A_DIM)
                stdscr.addstr(h - 2, mid + 1, hint[:w - mid - 2])
                stdscr.attroff(curses.A_DIM)

            # Status bar (avoid bottom-right corner bug)
            stdscr.attron(curses.A_REVERSE)
            try:
                stdscr.addstr(h - 1, 0, status_msg[:w - 1].ljust(w - 1))
            except curses.error:
                pass
            stdscr.attroff(curses.A_REVERSE)

            try:
                curses.curs_set(1 if input_mode else 0)
            except Exception:
                pass
            stdscr.refresh()
            time.sleep(0.03)

    except KeyboardInterrupt:
        pass
    finally:
        running = False
        thread.join(timeout=1)
        ser.close()
        curses.nocbreak()
        curses.echo()
        curses.curs_set(1)
        stdscr.keypad(False)
        curses.endwin()


def main():
    project_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(project_dir)

    print()
    print("========================================")
    print("  M5Claw Debug Console")
    print("========================================")

    # If port given as argument, use it directly
    if len(sys.argv) > 1:
        port = sys.argv[1]
    else:
        port = select_port(project_dir)

    if port:
        save_cached_port(project_dir, port)
        print(f"\nConnecting to {port}...\n")
        time.sleep(0.5)
        run_tui(port)
    else:
        print("No port selected.")


if __name__ == "__main__":
    main()
