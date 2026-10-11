"""Times how long the game takes to start: launches the exe in its folder, prints the milliseconds at which each line of
its output first appears (so a slow step stands out as a gap), stops once PATTERN shows up or after TIMEOUT seconds, and
ends only that process.

Usage: python tools/startup_time.py [--exe build/Rush2Recompiled.exe] [--until "Initializing recomp heap"] [--timeout 90]
"""
import argparse, os, subprocess, threading, time

ap = argparse.ArgumentParser()
ap.add_argument('--exe', default='build/Rush2Recompiled.exe')
ap.add_argument('--until', default='Initializing recomp heap')
ap.add_argument('--timeout', type=float, default=90)
args = ap.parse_args()

exe = os.path.abspath(args.exe)
start = time.monotonic()
proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
done = threading.Event()

def read():
    for raw in proc.stdout:
        line = raw.decode('utf-8', 'replace').rstrip()
        print('%7d ms  %s' % ((time.monotonic() - start) * 1000, line), flush=True)
        if args.until in line:
            done.set()
            return
    done.set()

threading.Thread(target=read, daemon=True).start()
done.wait(args.timeout)
print('%7d ms  (stopped)' % ((time.monotonic() - start) * 1000))
proc.kill()
