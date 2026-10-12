#!/usr/bin/env python3
"""RoboThings music server.

Shares the songs in a folder on your computer with the RoboThings S3 Mini
assistant on the same Wi-Fi, so you can say "Alexa, play Tum Hi Ho".

    python robothings_music_server.py "D:\\Music"

Needs Python 3.8+ and ffmpeg (https://ffmpeg.org) on PATH. No other packages.
The device finds this server by itself (UDP broadcast on port 47123); the
songs never leave your home network.
"""

import argparse
import difflib
import hashlib
import json
import os
import random
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import unicodedata
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

AUDIO_EXTS = {".mp3", ".m4a", ".aac", ".flac", ".wav", ".ogg", ".opus", ".wma"}
DISCOVERY_PORT = 47123
DISCOVERY_ASK = b"RTMUSIC?"

# Junk often found in downloaded file names.
NOISE = re.compile(
    r"\b(\d{2,3}\s*kbps|kbps|320|128|hq|hd|official|video|audio|lyrics?|full\s*song|"
    r"mp3|songs?\.pk|pagalworld|djmaza|mr\s*jatt|webmusic|wynk|www\.\S+|\S+\.(com|in|net|co))\b",
    re.IGNORECASE,
)


def normalize(text):
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    text = text.lower()
    text = NOISE.sub(" ", text)
    text = re.sub(r"[^a-z0-9]+", " ", text)
    return " ".join(text.split())


def pretty_title(path):
    stem = os.path.splitext(os.path.basename(path))[0]
    stem = re.sub(r"^\s*\d{1,3}\s*[-_. ]+\s*", "", stem)  # leading track number
    stem = re.sub(r"[_]+", " ", stem)
    stem = re.sub(r"\s*[\[(][^\])]*(kbps|www|\.com|\.in|songs)[^\])]*[\])]", "", stem, flags=re.I)
    return " ".join(stem.split()) or os.path.basename(path)


class Library:
    def __init__(self, folder):
        self.folder = os.path.abspath(folder)
        self.tracks = {}
        self.lock = threading.Lock()
        self.scan()

    def scan(self):
        tracks = {}
        for root, _dirs, files in os.walk(self.folder):
            for name in files:
                if os.path.splitext(name)[1].lower() not in AUDIO_EXTS:
                    continue
                path = os.path.join(root, name)
                rel = os.path.relpath(path, self.folder)
                track_id = hashlib.sha1(rel.encode("utf-8", "ignore")).hexdigest()[:12]
                title = pretty_title(path)
                folder_name = os.path.basename(os.path.dirname(path))
                if os.path.dirname(rel) == "":
                    folder_name = ""
                search_text = normalize(title + " " + folder_name)
                tracks[track_id] = {
                    "id": track_id,
                    "title": title,
                    "album": folder_name,
                    "path": path,
                    "key": search_text,
                    "words": set(search_text.split()),
                }
        with self.lock:
            self.tracks = tracks
        return len(tracks)

    def search(self, query, limit=5):
        q = normalize(query)
        with self.lock:
            tracks = list(self.tracks.values())
        if not q:
            return []
        q_words = set(q.split())
        scored = []
        for t in tracks:
            overlap = len(q_words & t["words"]) / max(1, len(q_words))
            # Partial word hits ("kesariya" vs "kesariyaa").
            fuzzy_hits = 0
            for w in q_words - t["words"]:
                if difflib.get_close_matches(w, t["words"], n=1, cutoff=0.8):
                    fuzzy_hits += 1
            overlap += 0.7 * fuzzy_hits / max(1, len(q_words))
            ratio = difflib.SequenceMatcher(None, q, t["key"]).ratio()
            contains = 0.3 if q in t["key"] else 0.0
            score = overlap * 0.6 + ratio * 0.3 + contains
            if score >= 0.25:
                scored.append((score, t))
        scored.sort(key=lambda item: item[0], reverse=True)
        return [self.public(t, s) for s, t in scored[:limit]]

    def random(self, exclude=None):
        with self.lock:
            ids = [i for i in self.tracks if i != exclude]
        if not ids:
            return None
        return self.public(self.tracks[random.choice(ids)])

    def get(self, track_id):
        with self.lock:
            return self.tracks.get(track_id)

    @staticmethod
    def public(t, score=None):
        out = {"id": t["id"], "title": t["title"]}
        if t["album"]:
            out["album"] = t["album"]
        if score is not None:
            out["score"] = round(score, 2)
        return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # stream until the connection closes
    library = None
    bitrate = "40k"

    def log_message(self, fmt, *args):
        sys.stdout.write("[%s] %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        params = parse_qs(url.query)
        lib = self.library
        if url.path == "/api/info":
            return self.send_json({"name": "RoboThings music server", "tracks": len(lib.tracks)})
        if url.path == "/api/search":
            query = params.get("q", [""])[0]
            limit = max(1, min(10, int(params.get("limit", ["5"])[0])))
            return self.send_json({"query": query, "results": lib.search(query, limit)})
        if url.path == "/api/random":
            track = lib.random(params.get("exclude", [None])[0])
            return self.send_json({"result": track})
        if url.path == "/api/rescan":
            return self.send_json({"tracks": lib.scan()})
        if url.path.startswith("/api/stream/"):
            return self.stream(url.path.rsplit("/", 1)[-1], params)
        return self.send_json({"error": "not found"}, 404)

    def stream(self, track_id, params):
        track = self.library.get(track_id)
        if track is None:
            return self.send_json({"error": "unknown track"}, 404)
        try:
            start = max(0, int(float(params.get("start", ["0"])[0])))
        except ValueError:
            start = 0
        # Opus in Ogg, mono 24 kHz, 60 ms frames: exactly what the device decodes.
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-ss", str(start), "-i", track["path"], "-vn",
            "-ac", "1", "-ar", "24000",
            "-c:a", "libopus", "-b:a", self.bitrate, "-application", "audio",
            "-frame_duration", "60", "-page_duration", "200000",
            "-f", "ogg", "pipe:1",
        ]
        print("Playing: %s (from %ss)" % (track["title"], start))
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "audio/ogg")
            self.send_header("X-Track-Title", track["title"].encode("ascii", "ignore").decode())
            self.end_headers()
            while True:
                chunk = proc.stdout.read(4096)
                if not chunk:
                    break
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            print("Stopped by device: %s" % track["title"])
        finally:
            proc.kill()
            proc.wait()


def discovery_responder(http_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", DISCOVERY_PORT))
    reply = ("RTMUSIC %d" % http_port).encode()
    while True:
        data, addr = sock.recvfrom(256)
        if data.strip() == DISCOVERY_ASK:
            sock.sendto(reply, addr)
            print("Device found the server: %s" % addr[0])


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    parser = argparse.ArgumentParser(description="Share a music folder with the RoboThings assistant.")
    parser.add_argument("folder", nargs="?", default=".", help="folder with your songs (searched recursively)")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--bitrate", default="40k", help="Opus bitrate sent to the device (default 40k)")
    args = parser.parse_args()

    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg was not found. Install it from https://ffmpeg.org and add it to PATH.")
    if not os.path.isdir(args.folder):
        sys.exit("Folder not found: %s" % args.folder)

    library = Library(args.folder)
    Handler.library = library
    Handler.bitrate = args.bitrate

    def rescan_loop():
        while True:
            time.sleep(120)
            library.scan()

    threading.Thread(target=rescan_loop, daemon=True).start()
    threading.Thread(target=discovery_responder, args=(args.port,), daemon=True).start()

    server = ThreadingHTTPServer(("", args.port), Handler)
    print("RoboThings music server")
    print("  songs: %d in %s" % (len(library.tracks), library.folder))
    print("  address: http://%s:%d  (the device finds it automatically)" % (lan_ip(), args.port))
    print("Say: \"Alexa, play <song name>\". Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
