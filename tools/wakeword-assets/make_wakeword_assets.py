#!/usr/bin/env python3
"""Build one assets image per wake word for voice switching (CI helper).

Run after `scripts/build.py robothings-s3-mini ...` has built the firmware. It
reuses the exact build_default_assets.py command CMake ran for generated_assets.bin
and only swaps the WakeNet model, so fonts and everything else stay identical.

    python tools/wakeword-assets/make_wakeword_assets.py OUTPUT_DIR

Writes OUTPUT_DIR/<model>.bin for every model that the ESP-SR component provides,
plus OUTPUT_DIR/index.json. Keep MODELS in sync with wake_word_switch.cc.
"""

import json
import os
import re
import shlex
import subprocess
import sys

MODELS = [
    ("Alexa", "wn9_alexa"),
    ("Hi ESP", "wn9_hiesp"),
    ("Jarvis", "wn9_jarvis_tts"),
    ("Computer", "wn9_computer_tts"),
    ("Sophia", "wn9_sophia_tts"),
    ("Mycroft", "wn9_mycroft_tts"),
    ("Hi Joy", "wn9_hijoy_tts"),
    ("Hi Jason", "wn9_hijason_tts2"),
    ("Hi Andy", "wn9_hiandy_tts2"),
    ("Hey Willow", "wn9_heywillow_tts"),
    ("Hey Wanda", "wn9_heywanda_tts"),
    ("Hey Ivy", "wn9_heyivy_tts2"),
    ("Hey Kira", "wn9_heykira_tts3"),
    ("Hi Lily", "wn9_hilili_tts"),
    ("Hi Telly", "wn9_hitelly_tts"),
    ("Hi Wall E", "wn9_hiwalle_tts2"),
    ("Nihao Xiaozhi", "wn9_nihaoxiaozhi_tts"),
]


def find_assets_command(build_dir):
    """Return (cwd, argv) of the build_default_assets.py call in build.ninja."""
    with open(os.path.join(build_dir, "build.ninja"), encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            # Only the "  COMMAND = cd ... && python build_default_assets.py ..." variable,
            # not the "build ...: CUSTOM_COMMAND ..." statement line.
            if not stripped.startswith("COMMAND = ") or "build_default_assets.py" not in stripped:
                continue
            command = stripped[len("COMMAND = "):]
            cwd = None
            for part in command.split(" && "):
                part = part.strip()
                if part.startswith("cd "):
                    cwd = shlex.split(part)[1]
                if "build_default_assets.py" in part:
                    return cwd, shlex.split(part)
    raise SystemExit("build_default_assets.py command not found in build.ninja")


def replace_arg(argv, flag, value):
    out = list(argv)
    if flag in out:
        out[out.index(flag) + 1] = value
    else:
        out += [flag, value]
    return out


def main():
    out_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "wakewords")
    build_dir = os.path.abspath("build")
    os.makedirs(out_dir, exist_ok=True)
    cwd, argv = find_assets_command(build_dir)
    print("assets command (cwd=%s): %s" % (cwd, " ".join(argv)))
    sdkconfig = argv[argv.index("--sdkconfig") + 1]
    model_root = argv[argv.index("--esp_sr_model_path") + 1] if "--esp_sr_model_path" in argv else ""
    with open(sdkconfig, encoding="utf-8") as f:
        base_lines = [l for l in f.read().splitlines() if not l.startswith("CONFIG_SR_WN_")]

    published = []
    for name, model in MODELS:
        if model_root and not os.path.isdir(os.path.join(model_root, "wakenet_model", model)):
            print("skip %s: model not in this ESP-SR version" % model)
            continue
        tmp_config = os.path.join(build_dir, "sdkconfig.wakeword")
        with open(tmp_config, "w", encoding="utf-8") as f:
            f.write("\n".join(base_lines + ["CONFIG_SR_WN_%s=y" % model.upper()]) + "\n")
        output = os.path.join(out_dir, model + ".bin")
        cmd = replace_arg(replace_arg(argv, "--sdkconfig", tmp_config), "--output", output)
        result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(output):
            print("skip %s: build failed\n%s%s" % (model, result.stdout[-800:], result.stderr[-800:]))
            continue
        with open(output, "rb") as f:
            if model.encode() not in f.read():
                print("skip %s: model missing from output" % model)
                os.remove(output)
                continue
        size = os.path.getsize(output)
        published.append({"name": name, "model": model, "size": size})
        print("ok %-22s %7d bytes" % (model, size))

    with open(os.path.join(out_dir, "index.json"), "w", encoding="utf-8") as f:
        json.dump(published, f, indent=2)
    if not published:
        raise SystemExit("no wake word assets were built")


if __name__ == "__main__":
    main()
