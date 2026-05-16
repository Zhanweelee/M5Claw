Import("env")
import configparser
import os


def quote_build_flag(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'\\"{escaped}\\"'


project_dir = env.subst("$PROJECT_DIR")
config_file = os.path.join(project_dir, "user_config.ini")

mapping = {
    "wifi_ssid": "USER_WIFI_SSID",
    "wifi_pass": "USER_WIFI_PASS",
    "wifi_ssid2": "USER_WIFI_SSID2",
    "wifi_pass2": "USER_WIFI_PASS2",
    "provider": "USER_PROVIDER",
    "provider_model": "USER_PROVIDER_MODEL",
    "provider_api_key": "USER_PROVIDER_API_KEY",
    "mimo_api_key": "USER_MIMO_KEY",
    "mimo_model": "USER_MIMO_MODEL",
    "deepseek_api_key": "USER_DEEPSEEK_KEY",
    "openai_api_key": "USER_OPENAI_KEY",
    "city": "USER_CITY",
    "assistant_name": "USER_ASSISTANT_NAME",
    "tts_provider": "USER_TTS_PROVIDER",
    "tts_key": "USER_TTS_KEY",
    "tts_model": "USER_TTS_MODEL",
    "tts_voice": "USER_TTS_VOICE",
    "stt_provider": "USER_STT_PROVIDER",
    "stt_key": "USER_STT_KEY",
    "stt_model": "USER_STT_MODEL",
    "bocha_api_key": "USER_BOCHA_KEY",
}

# Environment variable overrides (flash.py sets these)
env_override = {
    "M5CLAW_WIFI_SSID":        "wifi_ssid",
    "M5CLAW_WIFI_PASS":        "wifi_pass",
    "M5CLAW_WIFI_SSID2":       "wifi_ssid2",
    "M5CLAW_WIFI_PASS2":       "wifi_pass2",
    "M5CLAW_PROVIDER":         "provider",
    "M5CLAW_PROVIDER_MODEL":   "provider_model",
    "M5CLAW_PROVIDER_API_KEY": "provider_api_key",
    "M5CLAW_CITY":             "city",
    "M5CLAW_ASSISTANT_NAME":   "assistant_name",
    "M5CLAW_TTS_PROVIDER":     "tts_provider",
    "M5CLAW_TTS_KEY":          "tts_key",
    "M5CLAW_TTS_MODEL":        "tts_model",
    "M5CLAW_TTS_VOICE":        "tts_voice",
    "M5CLAW_STT_PROVIDER":     "stt_provider",
    "M5CLAW_STT_KEY":          "stt_key",
    "M5CLAW_STT_MODEL":        "stt_model",
    "M5CLAW_BOCHA_KEY":        "bocha_api_key",
}

flags = []
seen_keys = set()

# First, read from environment variables (higher priority)
for env_var, ini_key in env_override.items():
    val = os.environ.get(env_var, "").strip()
    if val and ini_key in mapping:
        macro_name = mapping[ini_key]
        flags.append(f"-D{macro_name}={quote_build_flag(val)}")
        seen_keys.add(ini_key)
        display = f"[{len(val)} chars]" if ("key" in ini_key or "token" in ini_key or "pass" in ini_key) else val
        print(f"  [env] {ini_key} = {display}")

# Then read from user_config.ini (only if not overridden by env)
if os.path.exists(config_file):
    print(f"[M5Claw] Loading config from {config_file}")
    cp = configparser.ConfigParser()
    cp.read(config_file, encoding="utf-8")

    for ini_key, macro_name in mapping.items():
        if ini_key in seen_keys:
            continue
        val = cp.get("user", ini_key, fallback="").strip()
        if val:
            flags.append(f"-D{macro_name}={quote_build_flag(val)}")
            display = f"[{len(val)} chars]" if ("key" in ini_key or "token" in ini_key or "pass" in ini_key) else val
            print(f"  {ini_key} = {display}")
elif not seen_keys:
    print("[M5Claw] No user_config.ini found, skipping build-time config")

if flags:
    env.Append(BUILD_FLAGS=flags)
    print(f"[M5Claw] Injected {len(flags)} config values")
