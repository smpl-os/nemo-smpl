#!/usr/bin/env python3
"""Verify saved folders survive a real process exit in a private profile."""

import os
import ctypes
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import wave


def send_key(inspector, title, key, env, control=False):
    xid = int(subprocess.check_output(
        [inspector, "--window-id", title], env=env, timeout=15))
    x11 = ctypes.CDLL("libX11.so.6")
    xtst = ctypes.CDLL("libXtst.so.6")
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
    x11.XStringToKeysym.restype = ctypes.c_ulong
    x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XKeysymToKeycode.restype = ctypes.c_uint
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    display = x11.XOpenDisplay(env["DISPLAY"].encode())
    if not display:
        raise RuntimeError("Cannot connect to the isolated X display")
    try:
        x11.XSetInputFocus(display, xid, 2, 0)
        code = x11.XKeysymToKeycode(display, x11.XStringToKeysym(key.encode()))
        modifier = x11.XKeysymToKeycode(display, x11.XStringToKeysym(b"Control_L"))
        if not code:
            raise RuntimeError(f"No keycode for {key}")
        if control:
            xtst.XTestFakeKeyEvent(display, modifier, True, 0)
        xtst.XTestFakeKeyEvent(display, code, True, 0)
        xtst.XTestFakeKeyEvent(display, code, False, 0)
        if control:
            xtst.XTestFakeKeyEvent(display, modifier, False, 0)
        x11.XSync(display, False)
    finally:
        x11.XCloseDisplay(display)


def main():
    if os.environ.get("NEMO_TEST_ISOLATED") != "1":
        return 77
    profile = Path(os.environ["NEMO_TEST_PROFILE"])
    executable = str(Path(sys.argv[1]).resolve())
    inspector = str(Path(sys.argv[2]).resolve())
    first = profile / "remember-first"
    second = profile / "remember-second"
    unplugged = profile / "unplugged-drive"
    first.mkdir()
    second.mkdir()
    unplugged.mkdir()
    env = os.environ.copy()
    env.update(
        GSETTINGS_BACKEND="keyfile", G_DEBUG="fatal-criticals",
        XDG_CURRENT_DESKTOP="X-Nemo-Test", GTK_USE_PORTAL="0",
        LC_ALL="C", LANGUAGE="C",
    )
    subprocess.run(
        ["gsettings", "set", "org.nemo.preferences", "show-full-path-titles", "true"],
        env=env, check=True, timeout=10,
    )
    log_path = profile / "window-session.log"
    process = None
    audio_server = None
    with log_path.open("w+") as log:
        try:
            cases = [
                ([str(first)], first, None),
                ([], first, None),
                ([str(second)], second, None),
                ([], None, "false"),
                ([], second, "true"),
                ([str(unplugged)], unplugged, None),
                ([], None, None),
                ([], None, None),
            ]
            for args, expected, restore in cases:
                if restore is not None:
                    subprocess.run(
                        ["gsettings", "set", "org.nemo.preferences",
                         "restore-tabs-on-startup", restore],
                        env=env, check=True, timeout=10,
                    )
                process = subprocess.Popen(
                    [executable, "--no-desktop", *args],
                    env=env, stdout=log, stderr=log,
                )
                title = f"{expected.name} - {expected}" if expected is not None else "Home"
                subprocess.run(
                    [inspector, "--wait-for-title", title], env=env,
                    check=True, timeout=15,
                )
                if process.poll() is not None:
                    raise RuntimeError(f"Nemo exited while opening {title}: {process.returncode}")
                subprocess.run(
                    [executable, "--quit"], env=env, stdout=log, stderr=log,
                    check=True, timeout=20,
                )
                if process.wait(timeout=10) != 0:
                    raise RuntimeError(f"Nemo exited with status {process.returncode}")
                if expected == unplugged:
                    unplugged.rmdir()
            if env.get("NEMO_TEST_GSTREAMER") == "1":
                track = first / "track-220.wav"
                with wave.open(str(track), "wb") as audio:
                    audio.setparams((2, 2, 44100, 0, "NONE", "not compressed"))
                    frames = b"".join(struct.pack(
                        "<hh", int(20000 * math.sin(2 * math.pi * 440 * i / 44100)),
                        int(20000 * math.sin(2 * math.pi * 3520 * i / 44100)),
                    ) for i in range(44100))
                    audio.writeframes(frames * 90)
                for index in range(240):
                    if index != 220:
                        (first / f"track-{index:03d}.wav").symlink_to(track.name)
                pulse = os.environ.get("NEMO_TEST_PULSE_EXE") or shutil.which("pulseaudio")
                if not pulse:
                    raise RuntimeError("Real F3 qualification requires pulseaudio for a private, clocked null sink")
                audio_env = env.copy()
                audio_env["PULSE_RUNTIME_PATH"] = str(profile / "pulse")
                library_path = os.environ.get("NEMO_TEST_PULSE_LIBRARY_PATH")
                if library_path:
                    audio_env["LD_LIBRARY_PATH"] = library_path
                socket = profile / "pulse" / "native"
                arguments = [
                    pulse, "--daemonize=no", "--use-pid-file=no", "--exit-idle-time=-1",
                    "--realtime=no", "--high-priority=no", "--disable-shm=yes",
                    "--log-target=stderr", "-n",
                    "--load=module-null-sink sink_name=nemo-qualification rate=44100",
                    f"--load=module-native-protocol-unix socket={socket} auth-anonymous=true",
                ]
                module_path = os.environ.get("NEMO_TEST_PULSE_MODULE_PATH")
                if module_path:
                    arguments.append(f"--dl-search-path={module_path}")
                audio_server = subprocess.Popen(arguments, env=audio_env, stdout=log, stderr=log)
                deadline = time.monotonic() + 10
                while not socket.exists():
                    if audio_server.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("Private clocked audio server failed to start")
                    time.sleep(0.02)
                alsa = profile / "asound.conf"
                alsa.write_text("pcm.!default { type null }\n", encoding="ascii")
                env.update(
                    ALSA_CONFIG_PATH=str(alsa), PULSE_SERVER=f"unix:{socket}",
                    PIPEWIRE_REMOTE="nemo-no-audio-server", JACK_DEFAULT_SERVER="nemo-no-jack",
                    JACK_NO_START_SERVER="1",
                )
                modes = ["stereo-spectrum", "spectrogram", "digital-rain"]
                if env.get("NEMO_TEST_PROJECTM") == "1":
                    modes.extend(("aurora-cathedral", "stellar-voyage", "earthflight"))
                media = [(track, mode, mode) for mode in modes]
                for codec in ("vp8", "vp9", "av1"):
                    video = first / f"qualification-{codec}.webm"
                    subprocess.run(
                        [inspector, "--encode-webm", codec, str(video)],
                        env=env, check=True, timeout=30,
                    )
                    media.append((video, "off", codec))
                recorder = shutil.which("parec")
                if not recorder:
                    raise RuntimeError("Actual normalization qualification requires parec")
                media.extend((track, "stereo-spectrum", case) for case in (
                    "normalization-first", "normalization-cached", "normalization-volume"))
                for selected, mode, case in media:
                    normalizing = case.startswith("normalization-")
                    volume = "0.5" if case == "normalization-volume" else "1.0" if normalizing else "0.23"
                    for key, value in (("media-player-volume", volume),
                                       ("media-player-muted", str(not normalizing).lower()),
                                       ("media-player-visual", mode),
                                       ("media-player-normalize", str(normalizing).lower())):
                        subprocess.run(
                            ["gsettings", "set", "org.nemo.preferences", key, value],
                            env=env, check=True, timeout=10,
                        )
                    process = subprocess.Popen(
                        [executable, "--no-desktop", "--select", str(selected)],
                        env=env, stdout=log, stderr=log,
                    )
                    title = f"{first.name} - {first}"
                    subprocess.run([inspector, "--wait-for-title", title], env=env, check=True, timeout=15)
                    # Mapping precedes the asynchronous --select directory load.
                    time.sleep(0.25)
                    send_key(inspector, title, "F3", env)
                    player_title = selected.name
                    subprocess.run([inspector, "--wait-for-title", player_title], env=env, check=True, timeout=15)
                    image = profile / f"player-native-{case}.png"
                    deadline = time.monotonic() + 10
                    while True:
                        colored, black = map(int, subprocess.check_output(
                            [inspector, "--capture-window", player_title, str(image)],
                            env=env, timeout=15,
                        ).split())
                        minimum_color = 100 if mode == "digital-rain" else 1000
                        if colored > minimum_color and (black > 100 or mode == "earthflight"):
                            break
                        if time.monotonic() > deadline:
                            output = os.environ.get("NEMO_TEST_VISUAL_OUTPUT")
                            if output:
                                shutil.copyfile(image, Path(output) / image.name)
                            raise RuntimeError(f"Actual F3 {case} did not render media: {colored=}, {black=}")
                        time.sleep(0.1)
                    output = os.environ.get("NEMO_TEST_VISUAL_OUTPUT")
                    if output:
                        shutil.copyfile(image, Path(output) / image.name)
                    if case == "stereo-spectrum":
                        send_key(inspector, player_title, "l", env, control=True)
                        time.sleep(0.25)
                        image = profile / "player-native-playlist.png"
                        subprocess.run(
                            [inspector, "--capture-window", player_title, str(image)],
                            env=env, check=True, timeout=15, stdout=subprocess.DEVNULL,
                        )
                        if output:
                            shutil.copyfile(image, Path(output) / image.name)
                        send_key(inspector, player_title, "Right", env, control=True)
                        subprocess.run(
                            [inspector, "--wait-for-title", "track-221.wav"],
                            env=env, check=True, timeout=15,
                        )
                        send_key(inspector, "track-221.wav", "Left", env, control=True)
                        subprocess.run(
                            [inspector, "--wait-for-title", player_title],
                            env=env, check=True, timeout=15,
                        )
                    if normalizing:
                        raw = profile / f"{case}.pcm"
                        with raw.open("wb") as samples:
                            capture = subprocess.Popen(
                                [recorder, "--raw", "--format=float32ne", "--rate=44100",
                                 "--channels=2", "--device=nemo-qualification.monitor"],
                                env=env, stdout=samples, stderr=log,
                            )
                            try:
                                time.sleep(3)
                                if capture.poll() is not None:
                                    raise RuntimeError("Private monitor recording stopped unexpectedly")
                            finally:
                                capture.terminate()
                                capture.wait(timeout=5)
                        lufs, peak = map(float, subprocess.check_output(
                            [inspector, "--measure-pcm", str(raw)], env=env, timeout=15,
                        ).split())
                        print(f"Actual F3 {case}: {lufs:.2f} LUFS, peak {peak:.6f}", flush=True)
                        if case == "normalization-first":
                            if not math.isfinite(lufs) or lufs <= -12:
                                raise RuntimeError(f"Uncached playback was unexpectedly normalized: {lufs}")
                        else:
                            target = -22.02 if case == "normalization-volume" else -16
                            if not math.isfinite(lufs) or abs(lufs - target) > 0.3:
                                raise RuntimeError(f"Actual normalized output missed {target} LUFS: {lufs}")
                            if not math.isfinite(peak) or peak > 10 ** (-0.9 / 20):
                                raise RuntimeError(f"Actual normalized output exceeded safe peak: {peak}")
                        if output:
                            shutil.copyfile(raw, Path(output) / raw.name)
                    send_key(inspector, title, "w", env, control=True)
                    if process.wait(timeout=10) != 0:
                        raise RuntimeError("Nemo did not naturally exit after closing the player's owner")
            # Overview bypasses directory loading; it must still release the
            # startup visibility gate and cancel a split pane's pending load.
            for left, right, active in (
                (["overview:///"], [], 0),
                ([first.as_uri(), "overview:///", second.as_uri()], [], 1),
                (["overview:///", first.as_uri()], ["overview:///"], 0),
            ):
                for key, value in (
                    ("saved-tabs-left", repr(left)),
                    ("saved-tabs-right", repr(right)),
                    ("saved-split-view", str(bool(right)).lower()),
                    ("saved-active-tab-left", str(active)),
                    ("saved-active-tab-right", "0"),
                    ("saved-active-pane", "1" if right else "0"),
                ):
                    subprocess.run(
                        ["gsettings", "set", "org.nemo.window-state", key, value],
                        env=env, check=True, timeout=10,
                    )
                process = subprocess.Popen(
                    [executable, "--no-desktop"], env=env, stdout=log, stderr=log,
                )
                subprocess.run(
                    [inspector, "--wait-for-title", "overview - overview:///"],
                    env=env, check=True, timeout=15,
                )
                # A second no-argument invocation must reuse the primary's
                # D-Bus service and create another mapped restored window.
                subprocess.run(
                    [executable, "--no-desktop"], env=env, stdout=log, stderr=log,
                    check=True, timeout=15,
                )
                subprocess.run(
                    [inspector, "--wait-for-title", "overview - overview:///", "2"],
                    env=env, check=True, timeout=15,
                )
                if process.poll() is not None:
                    raise RuntimeError("Primary Nemo exited during D-Bus reuse")
                subprocess.run(
                    [executable, "--no-desktop", str(first)],
                    env=env, stdout=log, stderr=log, check=True, timeout=15,
                )
                subprocess.run(
                    [inspector, "--wait-for-title", f"{first.name} - {first}"],
                    env=env, check=True, timeout=15,
                )
                subprocess.run(
                    [executable, "--quit"], env=env, stdout=log, stderr=log,
                    check=True, timeout=20,
                )
                if process.wait(timeout=10) != 0:
                    raise RuntimeError(f"Nemo exited with status {process.returncode}")
        except (RuntimeError, subprocess.SubprocessError):
            log.flush()
            print(log_path.read_text(), file=sys.stderr)
            raise
        finally:
            for child in (process, audio_server):
                if child is not None and child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
    print("Folder/Overview restoration, D-Bus reuse and configured real-process F3 playback passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
