#!/usr/bin/env python3
"""Splits a uisample.cpp profile of Nano Q's UI thread into where the time went.

    python nanoq/tools/uisample-split.py <profile.txt> [--top 15]

Each stack is put in the first bucket whose test matches, so the buckets add up. Percentages are of
one core over the sampled window, from each stack's cycles. "Self" lists show the heaviest functions
outside Windows' own wait stubs.
"""

import collections
import re
import sys

# First match wins, tested against the whole stack text, so the order is the hierarchy: within a
# paint, present and EndDraw come before the widget drawing that also sits in that stack.
BUCKETS = [
    ("wait tail (end of a burst)", r"(NtUserGetMessage|NtWaitForSingleObject|NtWaitForMultipleObjects|NtUserMsgWaitForMultipleObjectsEx)$"),
    ("present (swap chain)", r"(CDXGISwapChain|PresentImplCore|::Present\b|HwndPresenter|D3DKMTPresent|NtGdiDdDDIPresent)"),
    ("EndDraw / Flush (D2D batch to GPU)", r"(EndDraw|::Flush\b|FlushBatch|ProcessBatch|SubmitCommandCB|NtGdiDdDDISubmitCommand)"),
    ("BeginDraw / target setup", r"(BeginDraw|CreateCompatibleRenderTarget|CreateHwndRenderTarget|CreateSwapChain|GetDesktopDpi|Resize)"),
    ("widget drawing (D2D recording)", r"(DrawProbe|el::.*draw|elements::.*::draw|artist::canvas)"),
    ("BeginPaint / EndPaint", r"(NtUserBeginPaint|NtUserEndPaint|BeginPaint|EndPaint)"),
    ("UiSession::Pump", r"(UiSession::Pump|nanoq::ui::UiSession)"),
    ("message dispatch, other", r"(DispatchMessage|UserCallWinProc|CallWindowProc)"),
    ("message loop", r"(GetMessageW|PeekMessage|TranslateMessage)"),
]


def bucket_of(stack):
    for name, test in BUCKETS:
        if re.search(test, stack):
            return name
    return "other"


def main():
    args = sys.argv[1:]
    top = 15
    if "--top" in args:
        at = args.index("--top")
        top = int(args[at + 1])
        del args[at : at + 2]

    seconds = 10.0
    ghz = 3.793
    totals = collections.Counter()
    samples = collections.Counter()
    leaves = collections.Counter()
    ours = collections.Counter()

    with open(args[0], encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("#"):
                print(line.strip())
                found = re.search(r"seconds ([\d.]+).*tscGHz ([\d.]+)", line)
                if found:
                    seconds, ghz = float(found[1]), float(found[2])
                continue

            count, cycles, stack = line.rstrip("\n").split("\t", 2)
            cycles = int(cycles)
            name = bucket_of(stack)
            totals[name] += cycles
            samples[name] += int(count)
            frames = stack.split(";")
            if name != "wait tail (end of a burst)":
                leaves[frames[-1]] += cycles
                for frame in reversed(frames):
                    if frame.startswith("NanoQ") or frame.startswith("Soundshed"):
                        ours[frame] += cycles
                        break

    scale = 100.0 / (ghz * 1e9 * seconds)
    print(f"\n{'% of a core':>11}  {'samples':>7}  bucket")
    for name, cycles in totals.most_common():
        print(f"{cycles * scale:11.3f}  {samples[name]:7d}  {name}")
    print(f"{sum(totals.values()) * scale:11.3f}  {sum(samples.values()):7d}  total charged")

    print("\nHeaviest leaf functions:")
    for name, cycles in leaves.most_common(top):
        print(f"{cycles * scale:9.3f}  {name[:110]}")

    print("\nHeaviest of our own frames nearest the leaf:")
    for name, cycles in ours.most_common(top):
        print(f"{cycles * scale:9.3f}  {name[:110]}")


if __name__ == "__main__":
    main()
