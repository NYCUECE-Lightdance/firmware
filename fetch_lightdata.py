"""Download light show data for all players and save to lightdata.npz.
Run this once when you have internet access; control3.py loads from the cache.

Usage:
    python fetch_lightdata.py                     # eesa3 / LATEST
    python fetch_lightdata.py myuser              # custom user, LATEST
    python fetch_lightdata.py myuser 2025-04-30   # custom user + time
"""
import os
import sys

import numpy as np
import requests

USER = sys.argv[1] if len(sys.argv) > 1 else "eesa3"
TIME = sys.argv[2] if len(sys.argv) > 2 else "LATEST"

BASE = "https://eesa.dece.nycu.edu.tw/lightdance/api/items"

KEYS = ["time", "hat", "face", "chestL", "chestR", "armL", "armR", "tie",
        "belt", "gloveL", "gloveR", "legL", "legR", "shoeL", "shoeR", "board"]

OUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "lightdata.npz")


def main():
    url = f"{BASE}/{USER}/{TIME}"
    print(f"GET {url}")
    r = requests.get(url, timeout=60)
    r.raise_for_status()
    data = r.json()

    if "message" in data:
        print(f"Server: {data['message']}")
        sys.exit(1)

    players = data.get("players", [])
    if not players:
        print("No player data in response.")
        sys.exit(1)

    out = {}
    for i, entries in enumerate(players):
        rows = [[int(entry.get(k, 0)) for k in KEYS] for entry in entries]
        arr = (np.array(rows, dtype=np.uint32) if rows
               else np.zeros((0, 16), dtype=np.uint32))
        out[f"player_{i}"] = arr
        print(f"Player {i}: {len(arr)} frames")

    np.savez_compressed(OUT_PATH, **out)
    total = sum(len(v) for v in out.values())
    print(f"\nSaved {total} frames across {len(players)} players -> {OUT_PATH}")


if __name__ == "__main__":
    main()
