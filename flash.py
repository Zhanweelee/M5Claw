# -*- coding: utf-8 -*-
"""M5Claw flash: interactive config -> cache -> erase -> build -> upload"""
import subprocess
import sys
import os
import re
import json
import urllib.request
import urllib.error

CACHE_FILE = ".flash_cache.json"

PROVIDERS = [
    ("mimo",      "Xiaomi MiMo",    "mimo-v2-omni"),
    ("deepseek",  "DeepSeek",       "deepseek-v4-flash"),
    ("openai",    "OpenAI",         "gpt-4o"),
    ("anthropic", "Anthropic",      "claude-sonnet-4-6"),
    ("custom",    "Custom",         ""),
]

PROVIDER_MAP = {p[0]: p for p in PROVIDERS}

TTS_PROVIDERS = [
    ("",           "Use LLM provider TTS", "", ""),
    ("siliconflow", "SiliconFlow", "FunAudioLLM/CosyVoice2-0.5B", "FunAudioLLM/CosyVoice2-0.5B:alex"),
]

TTS_MODELS = {
    "siliconflow": [
        "FunAudioLLM/CosyVoice2-0.5B",
        "fnlp/MOSS-TTSD-v0.5",
    ],
}

TTS_VOICES = {
    "siliconflow": [
        ("FunAudioLLM/CosyVoice2-0.5B:alex",     "calm male"),
        ("FunAudioLLM/CosyVoice2-0.5B:benjamin", "deep male"),
        ("FunAudioLLM/CosyVoice2-0.5B:charles",  "magnetic male"),
        ("FunAudioLLM/CosyVoice2-0.5B:david",    "cheerful male"),
        ("FunAudioLLM/CosyVoice2-0.5B:anna",     "calm female"),
        ("FunAudioLLM/CosyVoice2-0.5B:bella",    "passionate female"),
        ("FunAudioLLM/CosyVoice2-0.5B:claire",   "gentle female"),
        ("FunAudioLLM/CosyVoice2-0.5B:diana",    "cheerful female"),
    ],
}

STT_PROVIDERS = [
    ("",           "Disabled (no voice input)", "", ""),
    ("siliconflow", "SiliconFlow", "FunAudioLLM/SenseVoiceSmall", ""),
]

STT_MODELS = {
    "siliconflow": [
        "FunAudioLLM/SenseVoiceSmall",
        "TeleAI/TeleSpeechASR",
    ],
}

MODEL_LIST_ENDPOINTS = {
    "deepseek":  {"url": "https://api.deepseek.com/models",        "needs_auth": True,  "auth_header": "Authorization"},
    "openai":    {"url": "https://api.openai.com/v1/models",       "needs_auth": True,  "auth_header": "Authorization"},
    "anthropic": {"url": "https://api.anthropic.com/v1/models",    "needs_auth": True,  "auth_header": "x-api-key"},
}


def fetch_models(provider_id, api_key=""):
    """Fetch available model IDs for a provider. Returns (models, error) tuple."""
    conf = MODEL_LIST_ENDPOINTS.get(provider_id)
    if not conf:
        return [], ""
    url = conf["url"]
    headers = {}
    if conf["needs_auth"] and api_key:
        headers[conf["auth_header"]] = api_key if conf["auth_header"] == "x-api-key" else f"Bearer {api_key}"
    elif conf["needs_auth"] and not api_key:
        return [], "API key required to list models for this provider"
    try:
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        return [], f"HTTP {e.code}: {e.reason}"
    except Exception as e:
        return [], str(e)
    models = []
    for item in data.get("data", []):
        mid = item.get("id", "")
        if mid:
            owned = item.get("owned_by", "")
            models.append((mid, owned))
    models.sort(key=lambda x: x[0])
    return models, ""


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
    print("========================================")
    print()

    if cache:
        print("Cached configuration found:")
        cached_prov = PROVIDER_MAP.get(cache.get("provider", ""), ("", "Unknown", ""))
        print(f"  WiFi:        {cache.get('wifi_ssid', '')}")
        if cache.get("wifi_ssid2"):
            print(f"  WiFi (alt):  {cache.get('wifi_ssid2', '')}")
        print(f"  Provider:    {cached_prov[1]}")
        print(f"  Model:       {cache.get('provider_model', '')}")
        print(f"  City:        {cache.get('city', '')}")
        if cache.get("assistant_name"):
            print(f"  Ast Name:    {cache.get('assistant_name', '')}")
        if cache.get("tts_provider"):
            tts_name = {p[0]: p[1] for p in TTS_PROVIDERS}.get(cache.get("tts_provider", ""), cache.get("tts_provider", ""))
            print(f"  TTS:         {tts_name}")
            print(f"  TTS Model:   {cache.get('tts_model', '')}")
            print(f"  TTS Voice:   {cache.get('tts_voice', '')}")
        if cache.get("stt_provider"):
            stt_name = {p[0]: p[1] for p in STT_PROVIDERS}.get(cache.get("stt_provider", ""), cache.get("stt_provider", ""))
            print(f"  STT:         {stt_name}")
            print(f"  STT Model:   {cache.get('stt_model', '')}")
        if cache.get("bocha_api_key"):
            print(f"  Bocha Key:   [{'*' * min(len(cache['bocha_api_key']), 8)}]")
        print()

        use_cached = prompt("Use existing config?", "Y").lower()
        if use_cached != "n" and use_cached != "no":
            config = dict(cache)
            pid, pname, _ = PROVIDER_MAP.get(config.get("provider", ""), ("", "Unknown", ""))
            print()
            print("-------- Configuration Summary --------")
            print(f"  WiFi:        {config['wifi_ssid']}")
            if config.get("wifi_ssid2"):
                print(f"  WiFi (alt):  {config['wifi_ssid2']}")
            print(f"  Provider:    {pname}")
            print(f"  Model:       {config['provider_model']}")
            print(f"  API Key:     [{'*' * min(len(config.get('provider_api_key', '')), 8)}]")
            print(f"  City:        {config['city']}")
            print(f"  Ast Name:    {config.get('assistant_name', 'M5Claw')}")
            if config.get("tts_provider"):
                tts_name2 = {p[0]: p[1] for p in TTS_PROVIDERS}.get(config["tts_provider"], config["tts_provider"])
                print(f"  TTS:         {tts_name2}")
                print(f"  TTS Model:   {config.get('tts_model', '')}")
                print(f"  TTS Voice:   {config.get('tts_voice', '')}")
                if config.get("tts_key"):
                    print(f"  TTS Key:     [{'*' * min(len(config['tts_key']), 8)}]")
            if config.get("stt_provider"):
                stt_name2 = {p[0]: p[1] for p in STT_PROVIDERS}.get(config["stt_provider"], config["stt_provider"])
                print(f"  STT:         {stt_name2}")
                print(f"  STT Model:   {config.get('stt_model', '')}")
                if config.get("stt_key"):
                    print(f"  STT Key:     [{'*' * min(len(config['stt_key']), 8)}]")
            if config.get("bocha_api_key"):
                print(f"  Bocha Key:   [{'*' * min(len(config['bocha_api_key']), 8)}]")
            print("----------------------------------------")

            ok = prompt("Proceed with flash? (Y/n)", "Y").lower()
            if ok and ok != "y" and ok != "yes":
                print("Cancelled")
                sys.exit(0)
            save_cache(project_dir, config)
            return config

    # Edit mode — show the hint about Enter for defaults
    print("(press Enter to use default/cached value)")
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
    elif pid in MODEL_LIST_ENDPOINTS:
        print()
        print(f"Fetching {pname} model list...")
        models, err = fetch_models(pid, provider_key)
        if err:
            print(f"  Failed: {err}")
            config["provider_model"] = prompt("Model name", cached_model or default_model)
        elif not models:
            print("  No models returned")
            config["provider_model"] = prompt("Model name", cached_model or default_model)
        else:
            print(f"Available {pname} models ({len(models)}):")
            for i, (mid, owned) in enumerate(models, 1):
                owner_tag = f" ({owned})" if owned else ""
                mark = " <-- cached" if mid == cached_model else ""
                print(f"  [{i:2d}] {mid}{owner_tag}{mark}")
            print("  [ 0] Enter model name manually")
            sel = prompt(f"Select model (0-{len(models)})", "0" if not cached_model else "")
            try:
                s = int(sel)
                if 1 <= s <= len(models):
                    config["provider_model"] = models[s - 1][0]
                else:
                    config["provider_model"] = prompt("Model name", cached_model or default_model)
            except ValueError:
                config["provider_model"] = prompt("Model name", cached_model or default_model)
    else:
        config["provider_model"] = prompt("Model name", cached_model or default_model)

    # Assistant name
    print()
    assistant_default = cache.get("assistant_name", "M5Claw")
    assistant_name = prompt("Assistant name (identity in system prompt)", assistant_default)
    config["assistant_name"] = assistant_name if assistant_name else "M5Claw"

    # City
    config["city"] = prompt("City", cache.get("city", "Beijing"))

    # ── TTS Configuration ──
    print()
    print("TTS (Text-to-Speech) — separate provider for voice output")
    print("Available TTS providers:")
    for i, (tid, tname, tmodel, tvoice) in enumerate(TTS_PROVIDERS):
        if i == 0:
            print(f"  [0] {tname}")
        else:
            print(f"  [{i}] {tname}  (default model: {tmodel}, voice: {tvoice})")

    cached_tts_provider = cache.get("tts_provider", "")
    default_tts_idx = 0
    for i, (tid, _, _, _) in enumerate(TTS_PROVIDERS):
        if tid == cached_tts_provider:
            default_tts_idx = i
            break

    tts_input = prompt(f"TTS provider (0-{len(TTS_PROVIDERS)-1})", str(default_tts_idx))
    try:
        tts_idx = int(tts_input)
        if tts_idx < 0 or tts_idx >= len(TTS_PROVIDERS):
            tts_idx = 0
    except ValueError:
        tts_idx = 0

    tts_pid, tts_pname, tts_default_model, tts_default_voice = TTS_PROVIDERS[tts_idx]
    config["tts_provider"] = tts_pid

    if tts_pid:
        # TTS API Key
        cached_tts_key = cache.get("tts_key", "")
        tts_key = prompt(
            f"{tts_pname} API Key (empty=use LLM key)",
            cached_tts_key,
            sensitive=True
        )
        config["tts_key"] = tts_key

        # TTS Model
        available_models = TTS_MODELS.get(tts_pid, [tts_default_model])
        cached_tts_model = cache.get("tts_model", "")
        print()
        print(f"Available {tts_pname} TTS models:")
        for i, m in enumerate(available_models, 1):
            mark = " <-- cached" if m == cached_tts_model else ""
            print(f"  [{i}] {m}{mark}")
        print("  [0] Enter model name manually")
        model_sel = prompt(f"Select TTS model (0-{len(available_models)})",
                          "0" if not cached_tts_model else "")
        try:
            ms = int(model_sel)
            if 1 <= ms <= len(available_models):
                config["tts_model"] = available_models[ms - 1]
            else:
                config["tts_model"] = prompt("TTS model name", cached_tts_model or tts_default_model)
        except ValueError:
            config["tts_model"] = prompt("TTS model name", cached_tts_model or tts_default_model)

        # TTS Voice
        available_voices = TTS_VOICES.get(tts_pid, [])
        cached_tts_voice = cache.get("tts_voice", "")
        print()
        print(f"Available {tts_pname} voices:")
        for i, (vid, vdesc) in enumerate(available_voices, 1):
            mark = " <-- cached" if vid == cached_tts_voice else ""
            print(f"  [{i}] {vid}  ({vdesc}){mark}")
        print("  [0] Enter voice name manually")
        voice_sel = prompt(f"Select TTS voice (0-{len(available_voices)})",
                          "0" if not cached_tts_voice else "")
        try:
            vs = int(voice_sel)
            if 1 <= vs <= len(available_voices):
                config["tts_voice"] = available_voices[vs - 1][0]
            else:
                config["tts_voice"] = prompt("TTS voice", cached_tts_voice or tts_default_voice)
        except ValueError:
            config["tts_voice"] = prompt("TTS voice", cached_tts_voice or tts_default_voice)
    else:
        config["tts_key"] = ""
        config["tts_model"] = ""
        config["tts_voice"] = ""

    # ── STT Configuration ──
    print()
    print("STT (Speech-to-Text) — voice transcription for providers without native audio input")
    print("Available STT providers:")
    for i, (sid, sname, smodel, _) in enumerate(STT_PROVIDERS):
        if i == 0:
            print(f"  [0] {sname}")
        else:
            print(f"  [{i}] {sname}  (default model: {smodel})")

    cached_stt_provider = cache.get("stt_provider", "")
    default_stt_idx = 0
    for i, (sid, _, _, _) in enumerate(STT_PROVIDERS):
        if sid == cached_stt_provider:
            default_stt_idx = i
            break

    stt_input = prompt(f"STT provider (0-{len(STT_PROVIDERS)-1})", str(default_stt_idx))
    try:
        stt_idx = int(stt_input)
        if stt_idx < 0 or stt_idx >= len(STT_PROVIDERS):
            stt_idx = 0
    except ValueError:
        stt_idx = 0

    stt_pid, stt_pname, stt_default_model, _ = STT_PROVIDERS[stt_idx]
    config["stt_provider"] = stt_pid

    if stt_pid:
        # STT API Key
        cached_stt_key = cache.get("stt_key", "")
        stt_key = prompt(
            f"{stt_pname} API Key (empty=use LLM key)",
            cached_stt_key,
            sensitive=True
        )
        config["stt_key"] = stt_key

        # STT Model
        available_stt_models = STT_MODELS.get(stt_pid, [stt_default_model])
        cached_stt_model = cache.get("stt_model", "")
        print()
        print(f"Available {stt_pname} STT models:")
        for i, m in enumerate(available_stt_models, 1):
            mark = " <-- cached" if m == cached_stt_model else ""
            print(f"  [{i}] {m}{mark}")
        print("  [0] Enter model name manually")
        stt_model_sel = prompt(f"Select STT model (0-{len(available_stt_models)})",
                               "0" if not cached_stt_model else "")
        try:
            sms = int(stt_model_sel)
            if 1 <= sms <= len(available_stt_models):
                config["stt_model"] = available_stt_models[sms - 1]
            else:
                config["stt_model"] = prompt("STT model name", cached_stt_model or stt_default_model)
        except ValueError:
            config["stt_model"] = prompt("STT model name", cached_stt_model or stt_default_model)
    else:
        config["stt_key"] = ""
        config["stt_model"] = ""

    # ── Bocha Web Search Key ──
    print()
    print("Web Search — Bocha AI (get free key at https://open.bochaai.com)")
    cached_bocha = cache.get("bocha_api_key", "")
    bocha_key = prompt("Bocha API Key (empty=skip, web_search disabled)", cached_bocha, sensitive=True)
    config["bocha_api_key"] = bocha_key

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
    print(f"  Ast Name:    {config.get('assistant_name', 'M5Claw')}")
    if config.get("tts_provider"):
        tts_name = {p[0]: p[1] for p in TTS_PROVIDERS}.get(config["tts_provider"], config["tts_provider"])
        print(f"  TTS:         {tts_name}")
        print(f"  TTS Model:   {config.get('tts_model', '')}")
        print(f"  TTS Voice:   {config.get('tts_voice', '')}")
        if config.get("tts_key"):
            print(f"  TTS Key:     [{'*' * min(len(config['tts_key']), 8)}]")
    if config.get("stt_provider"):
        stt_name = {p[0]: p[1] for p in STT_PROVIDERS}.get(config["stt_provider"], config["stt_provider"])
        print(f"  STT:         {stt_name}")
        print(f"  STT Model:   {config.get('stt_model', '')}")
        if config.get("stt_key"):
            print(f"  STT Key:     [{'*' * min(len(config['stt_key']), 8)}]")
    if config.get("bocha_api_key"):
        print(f"  Bocha Key:   [{'*' * min(len(config['bocha_api_key']), 8)}]")
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

    blocked = {"debug-console", "Bluetooth-Incoming-Port"}

    def scan_ports_pyserial():
        ports = []
        try:
            import serial.tools.list_ports
            for p in serial.tools.list_ports.comports():
                if any(b in p.device for b in blocked):
                    continue
                if "bluetooth" in p.description.lower() and p.vid is None:
                    continue
                ports.append(p.device)
        except Exception:
            pass
        return sorted(ports)

    ports = scan_ports_pyserial()

    # Fallback to pio device list
    if not ports:
        try:
            raw = subprocess.check_output(["pio", "device", "list"])
            try:
                out = raw.decode("gbk")
            except UnicodeDecodeError:
                out = raw.decode("utf-8", errors="replace")
            raw_ports = set(
                re.findall(r"^(?:COM\d+|/dev/(?:cu|tty)\.(?:usbmodem|usbserial|usb|ACM)\S*)",
                           out, re.MULTILINE)
            )
            ports = sorted(p for p in raw_ports if "debug" not in p and "Bluetooth" not in p)
        except Exception:
            pass

    if not ports:
        print("No serial ports detected. Connect the device and try again.")
        sys.exit(1)

    # 2. Select port
    if len(ports) == 1:
        port = ports[0]
        print(f"\nAuto-selected port: {port}")
    else:
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
        ("M5CLAW_ASSISTANT_NAME",   "assistant_name"),
        ("M5CLAW_TTS_PROVIDER",     "tts_provider"),
        ("M5CLAW_TTS_KEY",          "tts_key"),
        ("M5CLAW_TTS_MODEL",        "tts_model"),
        ("M5CLAW_TTS_VOICE",        "tts_voice"),
        ("M5CLAW_STT_PROVIDER",     "stt_provider"),
        ("M5CLAW_STT_KEY",          "stt_key"),
        ("M5CLAW_STT_MODEL",        "stt_model"),
        ("M5CLAW_BOCHA_KEY",        "bocha_api_key"),
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
    if config.get("tts_provider"):
        print(f"  TTS: {config['tts_provider']}, Model: {config.get('tts_model', 'default')}, Voice: {config.get('tts_voice', 'default')}")
    if config.get("stt_provider"):
        print(f"  STT: {config['stt_provider']}, Model: {config.get('stt_model', 'default')}")
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
