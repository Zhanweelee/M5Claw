# -*- coding: utf-8 -*-
"""M5Claw flash: interactive config -> cache -> erase -> build -> upload"""
import subprocess
import sys
import os
import re
import json

CACHE_FILE = ".flash_cache.json"

PROVIDERS = [
    ("mimo",     "Xiaomi MiMo",   "mimo-v2-omni"),
    ("deepseek", "DeepSeek",      "deepseek-v4-flash"),
    ("openai",   "OpenAI",        "gpt-4o"),
    ("custom",   "Custom",        ""),
]

PROVIDER_MAP = {p[0]: p for p in PROVIDERS}


def load_cache(project_dir):
    path = os.path.join(project_dir, CACHE_FILE)
    if os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        except (json.JSONDecodeError, IOError):
            pass
    return {}


def save_cache(project_dir, config):
    path = os.path.join(project_dir, CACHE_FILE)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2, ensure_ascii=False)


def prompt(label, default="", sensitive=False):
    if default and not sensitive:
        prompt_str = f"{label} [{default}]: "
    elif default and sensitive:
        prompt_str = f"{label} [{len(default)} chars set]: "
    else:
        prompt_str = f"{label}: "
    try:
        val = input(prompt_str).strip()
    except (EOFError, KeyboardInterrupt):
        print("\nCancelled")
        sys.exit(0)
    return val if val else default


def interactive_config(project_dir):
    cache = load_cache(project_dir)
    config = {}

    print()
    print("========================================")
    print("  Device Configuration")
    print("  (press Enter to use default/cached value)")
    print("========================================")
    print()

    # WiFi (primary)
    config["wifi_ssid"] = prompt("WiFi SSID", cache.get("wifi_ssid", ""))
    config["wifi_pass"] = prompt("WiFi Password", cache.get("wifi_pass", ""), sensitive=True)

    # WiFi2 (backup, optional)
    wifi2_ssid = prompt("Backup WiFi SSID (optional)", cache.get("wifi_ssid2", ""))
    config["wifi_ssid2"] = wifi2_ssid
    if wifi2_ssid:
        config["wifi_pass2"] = prompt("Backup WiFi Password", cache.get("wifi_pass2", ""), sensitive=True)
    else:
        config["wifi_pass2"] = ""

    # Provider
    print()
    print("Available LLM providers:")
    for i, (pid, name, model) in enumerate(PROVIDERS, 1):
        default_note = f" (default model: {model})" if model else " (manual model entry)"
        print(f"  [{i}] {name}{default_note}")

    cached_provider = cache.get("provider", "mimo")
    default_idx = 1
    for i, (pid, _, _) in enumerate(PROVIDERS):
        if pid == cached_provider:
            default_idx = i + 1
            break

    prov_input = prompt(f"Provider (1-{len(PROVIDERS)})", str(default_idx))
    try:
        idx = int(prov_input) - 1
        if idx < 0 or idx >= len(PROVIDERS):
            idx = 0
    except ValueError:
        idx = 0

    pid, pname, default_model = PROVIDERS[idx]
    config["provider"] = pid

    # API Key
    cached_key = cache.get("provider_api_key", "")
    provider_key = prompt(
        f"{pname} API Key",
        cached_key,
        sensitive=True
    )
    config["provider_api_key"] = provider_key

    # Model
    cached_model = cache.get("provider_model", "")
    if pid == "custom":
        config["provider_model"] = prompt("Model name", cached_model or "")
    else:
        config["provider_model"] = prompt("Model name", cached_model or default_model)

    # City
    config["city"] = prompt("City", cache.get("city", "Beijing"))

    # Show summary
    print()
    print("-------- Configuration Summary --------")
    print(f"  WiFi:        {config['wifi_ssid']}")
    if config.get("wifi_ssid2"):
        print(f"  WiFi (alt):  {config['wifi_ssid2']}")
    print(f"  Provider:    {pname}")
    print(f"  Model:       {config['provider_model']}")
    print(f"  API Key:     [{'*' * min(len(config['provider_api_key']), 8)}]")
    print(f"  City:        {config['city']}")
    print("----------------------------------------")

    ok = prompt("Proceed with flash? (Y/n)", "Y").lower()
    if ok and ok != "y" and ok != "yes":
        print("Cancelled")
        sys.exit(0)

    # Save cache for next time
    save_cache(project_dir, config)

    return config


def main():
    project_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(project_dir)

    print()
    print("========================================")
    print("  M5Claw Flash Tool")
    print("========================================")

    # 1. Scan ports
    print()
    print("[1/5] Scanning serial ports...")
    try:
        raw = subprocess.check_output(["pio", "device", "list"])
        try:
            out = raw.decode("gbk")
        except UnicodeDecodeError:
            out = raw.decode("utf-8", errors="replace")
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("Error: could not run pio. Is PlatformIO installed?")
        sys.exit(1)

    # Match Windows COM ports and Unix /dev/tty* /dev/cu* serial devices
    raw_ports = set(
        re.findall(r"^(?:COM\d+|/dev/(?:cu|tty)\.(?:usbmodem|usbserial|usb|ACM)\S*)",
                   out, re.MULTILINE)
    )
    # Filter out debug and Bluetooth pseudo-devices
    ports = sorted(
        p for p in raw_ports
        if "debug" not in p and "Bluetooth" not in p
    )
    if not ports:
        print("No COM ports detected. Connect the device and try again.")
        sys.exit(1)

    # 2. Select port
    print()
    print("Available ports:")
    for i, p in enumerate(ports, 1):
        print(f"  [{i}] {p}")
    print("  [0] Exit")
    print()

    while True:
        try:
            sel = input(f"Select port (1-{len(ports)}): ").strip()
        except (EOFError, KeyboardInterrupt):
            print("Cancelled")
            sys.exit(0)
        if sel == "0":
            print("Cancelled")
            sys.exit(0)
        try:
            idx = int(sel)
            if 1 <= idx <= len(ports):
                port = ports[idx - 1]
                break
        except ValueError:
            pass
        print("Invalid input, try again")

    print(f"Selected: {port}")

    # 3. Interactive config
    config = interactive_config(project_dir)

    # Build environment variables for load_config.py
    build_env = os.environ.copy()
    for env_name, cfg_key in [
        ("M5CLAW_WIFI_SSID",        "wifi_ssid"),
        ("M5CLAW_WIFI_PASS",        "wifi_pass"),
        ("M5CLAW_WIFI_SSID2",       "wifi_ssid2"),
        ("M5CLAW_WIFI_PASS2",       "wifi_pass2"),
        ("M5CLAW_PROVIDER",         "provider"),
        ("M5CLAW_PROVIDER_MODEL",   "provider_model"),
        ("M5CLAW_PROVIDER_API_KEY", "provider_api_key"),
        ("M5CLAW_CITY",             "city"),
    ]:
        val = config.get(cfg_key, "").strip()
        if val:
            build_env[env_name] = val

    # 4. Erase
    print()
    print("[2/5] Erasing flash...")
    if subprocess.run(["pio", "pkg", "exec", "--", "esptool.py",
                       "--port", port, "erase_flash"]).returncode != 0:
        print("Error: erase failed. Hold G0, press RST, release G0 to enter download mode.")
        sys.exit(1)
    print("Erase complete")

    # 5. Build
    print()
    print("[3/5] Building...")
    if config.get("provider"):
        print(f"  Provider: {config['provider']}, Model: {config.get('provider_model', 'default')}")
    if subprocess.run(["pio", "run"], env=build_env).returncode != 0:
        print("Error: build failed")
        sys.exit(1)
    print("Build complete")

    # 6. Upload firmware
    print()
    print(f"[4/5] Uploading firmware to {port}...")
    if subprocess.run(["pio", "run", "-t", "upload", "--upload-port", port],
                      env=build_env).returncode != 0:
        print("Error: firmware upload failed")
        sys.exit(1)
    print("Firmware uploaded")

    # 7. Upload SPIFFS
    print()
    print("[5/5] Uploading SPIFFS data...")
    if subprocess.run(["pio", "run", "-t", "uploadfs", "--upload-port", port],
                      env=build_env).returncode != 0:
        print("Warning: SPIFFS upload failed")
    else:
        print("SPIFFS uploaded")

    provider_name = PROVIDER_MAP.get(config.get("provider", ""), ("", "Unknown", ""))[1]
    print()
    print("========================================")
    print("  Flash complete! Disconnect USB and press RST to reboot.")
    print(f"  Default provider: {provider_name}")
    print(f"  Config cached to {CACHE_FILE}")
    print("========================================")
    print()


if __name__ == "__main__":
    main()
