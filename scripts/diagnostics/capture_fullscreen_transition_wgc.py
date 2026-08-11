"""Record an F11 transition with Windows Graphics Capture and a fixed clock.

Windows Graphics Capture stays attached while the target app rebuilds its
borderless window.  Incoming compositor frames are sampled on an independent
clock, so a capture pause is represented by repeated source-frame ids instead
of disappearing from the timeline.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import json
import statistics
import subprocess
import sys
import threading
import time
import types
from pathlib import Path

import numpy as np

# windows-capture imports OpenCV only for its optional save_as_image method.
# The transition recorder consumes its NumPy view directly.
sys.modules.setdefault("cv2", types.SimpleNamespace())
from windows_capture import WindowsCapture  # noqa: E402


KEYEVENTF_KEYUP = 0x0002
VK_F11 = 0x7A


def window_for_process(process_id: int) -> int:
    user32 = ctypes.windll.user32
    result = 0
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def visit(hwnd: int, _: int) -> bool:
        nonlocal result
        candidate_pid = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(candidate_pid))
        if candidate_pid.value == process_id and user32.IsWindowVisible(hwnd):
            result = hwnd
            return False
        return True

    user32.EnumWindows(callback_type(visit), 0)
    if not result:
        raise RuntimeError(f"No visible window belongs to process {process_id}")
    return result


def send_f11(process_id: int) -> None:
    user32 = ctypes.windll.user32
    hwnd = window_for_process(process_id)
    user32.SetForegroundWindow(hwnd)
    user32.keybd_event(VK_F11, 0, 0, 0)
    user32.keybd_event(VK_F11, 0, KEYEVENTF_KEYUP, 0)


def wait_until(target: float) -> None:
    while True:
        remaining = target - time.perf_counter()
        if remaining <= 0:
            return
        if remaining > 0.002:
            time.sleep(remaining - 0.001)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--output-prefix", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--trigger-at", type=float, default=1.5)
    parser.add_argument("--downsample", type=int, default=4)
    args = parser.parse_args()

    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    video_path = args.output_prefix.with_suffix(".mkv")
    csv_path = args.output_prefix.with_suffix(".csv")

    frame_lock = threading.Lock()
    first_frame = threading.Event()
    capture_closed = threading.Event()
    latest: dict[str, object] = {}
    source_sequence = 0

    capture = WindowsCapture(
        cursor_capture=False,
        draw_border=False,
        monitor_index=None,
        minimum_update_interval=max(1, round(1000 / (args.fps * 2))),
    )

    @capture.event
    def on_frame_arrived(frame, _capture_control) -> None:
        nonlocal source_sequence
        source = frame.frame_buffer
        sampled = np.ascontiguousarray(source[:: args.downsample, :: args.downsample, :3])
        with frame_lock:
            source_sequence += 1
            latest["pixels"] = sampled
            latest["sequence"] = source_sequence
            latest["timespan"] = int(frame.timespan)
            latest["received_at"] = time.perf_counter()
            latest["source_size"] = (int(frame.width), int(frame.height))
        first_frame.set()

    @capture.event
    def on_closed() -> None:
        capture_closed.set()

    capture_control = capture.start_free_threaded()
    if not first_frame.wait(timeout=5):
        capture_control.stop()
        capture_control.wait()
        raise RuntimeError("Windows Graphics Capture did not provide an initial frame")

    with frame_lock:
        initial_pixels = latest["pixels"]
        source_size = latest["source_size"]
    assert isinstance(initial_pixels, np.ndarray)
    output_height, output_width = initial_pixels.shape[:2]

    ffmpeg = subprocess.Popen(
        [
            str(args.ffmpeg),
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "rawvideo",
            "-pixel_format",
            "bgr24",
            "-video_size",
            f"{output_width}x{output_height}",
            "-framerate",
            str(args.fps),
            "-i",
            "-",
            "-an",
            "-c:v",
            "h264_nvenc",
            "-preset",
            "p1",
            "-tune",
            "ll",
            "-rc",
            "constqp",
            "-qp",
            "14",
            str(video_path),
        ],
        stdin=subprocess.PIPE,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    if ffmpeg.stdin is None:
        capture_control.stop()
        capture_control.wait()
        raise RuntimeError("Unable to open the FFmpeg input pipe")

    frame_total = round(args.duration * args.fps)
    trigger_frame = round(args.trigger_at * args.fps)
    records: list[dict[str, float | int]] = []
    event_elapsed_ms = -1.0
    ctypes.windll.winmm.timeBeginPeriod(1)
    start = time.perf_counter() + 0.2
    try:
        for index in range(frame_total):
            target = start + index / args.fps
            wait_until(target)
            if index == trigger_frame:
                send_f11(args.pid)
                event_elapsed_ms = (time.perf_counter() - start) * 1000

            sample_time = time.perf_counter()
            with frame_lock:
                pixels = latest["pixels"]
                sequence = int(latest["sequence"])
                timespan = int(latest["timespan"])
                received_at = float(latest["received_at"])
            assert isinstance(pixels, np.ndarray)
            ffmpeg.stdin.write(pixels.tobytes())
            max_channel = pixels.max(axis=2)
            records.append(
                {
                    "frame": index,
                    "target_ms": index * 1000 / args.fps,
                    "sample_ms": (sample_time - start) * 1000,
                    "source_sequence": sequence,
                    "source_timespan": timespan,
                    "source_age_ms": (sample_time - received_at) * 1000,
                    "mean_bgr": float(pixels.mean()),
                    "dark_fraction": float(np.mean(max_channel < 12)),
                }
            )
    finally:
        ctypes.windll.winmm.timeEndPeriod(1)
        ffmpeg.stdin.close()
        ffmpeg.wait(timeout=20)
        capture_control.stop()
        capture_control.wait()

    with csv_path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=records[0].keys())
        writer.writeheader()
        writer.writerows(records)

    schedule_errors = [
        float(row["sample_ms"]) - float(row["target_ms"]) for row in records
    ]
    source_gaps = [
        records[index]["source_sequence"] == records[index - 1]["source_sequence"]
        for index in range(1, len(records))
    ]
    print(
        json.dumps(
            {
                "desktop": source_size,
                "recording": [output_width, output_height],
                "fps": args.fps,
                "frames": frame_total,
                "event_frame": trigger_frame,
                "event_elapsed_ms": event_elapsed_ms,
                "source_frames": source_sequence,
                "repeated_samples": sum(source_gaps),
                "schedule_error_ms_mean": statistics.mean(schedule_errors),
                "schedule_error_ms_max": max(schedule_errors),
                "video": str(video_path),
                "timeline": str(csv_path),
            },
            ensure_ascii=False,
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
