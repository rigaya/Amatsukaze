#!/usr/bin/env python3

"""ローカル更新テスト用のGitHub Releases API互換サーバー。"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from http import HTTPStatus
from pathlib import Path
from urllib.parse import quote, unquote, urlparse

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765
DIGEST_PREFIX = "sha256:"
HASH_CHUNK_BYTES = 1024 * 1024
MAX_PORT = 65535
LATEST_RELEASE_PATTERN = re.compile(r"^/repos/[^/]+/[^/]+/releases/latest$")
RELEASE_LIST_PATTERN = re.compile(r"^/repos/[^/]+/[^/]+/releases$")
DOWNLOAD_PREFIX = "/download/"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="GitHub Releases API互換のローカル更新テストサーバー"
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="待受アドレス")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="待受ポート")
    parser.add_argument("--release-dir", required=True, type=Path, help="アセット格納先")
    parser.add_argument("--version", required=True, help="返すリリースバージョン")
    return parser.parse_args()


def calculate_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(HASH_CHUNK_BYTES):
            digest.update(chunk)
    return DIGEST_PREFIX + digest.hexdigest()


def build_release(release_dir: Path, version: str, base_url: str) -> tuple[dict, dict[str, Path]]:
    asset_paths = {
        path.name: path
        for path in sorted(release_dir.iterdir())
        if path.is_file() and not path.is_symlink()
    }
    if not asset_paths:
        raise ValueError(f"アセットが見つかりません: {release_dir}")

    assets = []
    for name, path in asset_paths.items():
        size = path.stat().st_size
        digest = calculate_digest(path)
        assets.append(
            {
                "name": name,
                "browser_download_url": f"{base_url}{DOWNLOAD_PREFIX.lstrip('/')}{quote(name)}",
                "size": size,
                "digest": digest,
            }
        )
        print(f"[mock_release] asset={name} size={size} digest={digest}", flush=True)

    release = {
        "tag_name": version,
        "html_url": f"{base_url}release/{quote(version)}",
        "published_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "assets": assets,
    }
    return release, asset_paths


class ReleaseRequestHandler(BaseHTTPRequestHandler):
    release: dict
    asset_paths: dict[str, Path]

    # repository名はテスト対象ごとに異なるが、全て同じモックリリースを返す。
    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if LATEST_RELEASE_PATTERN.fullmatch(parsed.path):
            self.send_json(self.release)
            return
        if RELEASE_LIST_PATTERN.fullmatch(parsed.path):
            self.send_json([self.release])
            return
        if parsed.path.startswith(DOWNLOAD_PREFIX):
            self.send_asset(unquote(parsed.path[len(DOWNLOAD_PREFIX) :]))
            return
        self.send_payload(HTTPStatus.NOT_FOUND, b"not found\n", "text/plain; charset=utf-8")

    def send_json(self, value: object) -> None:
        payload = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_payload(HTTPStatus.OK, payload, "application/json; charset=utf-8")

    def send_asset(self, name: str) -> None:
        path = self.asset_paths.get(name)
        if path is None or "/" in name or "\\" in name:
            self.send_payload(
                HTTPStatus.NOT_FOUND,
                b"asset not found\n",
                "text/plain; charset=utf-8",
            )
            return

        size = path.stat().st_size
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        sent = 0
        try:
            with path.open("rb") as stream:
                while chunk := stream.read(HASH_CHUNK_BYTES):
                    self.wfile.write(chunk)
                    sent += len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.log_request_result(HTTPStatus.OK, sent)

    def send_payload(self, status: HTTPStatus, payload: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.log_request_result(status, len(payload))

    def log_request_result(self, status: HTTPStatus, byte_count: int) -> None:
        print(
            f"[mock_release] path={self.path} status={status} bytes={byte_count}",
            flush=True,
        )

    def log_message(self, format_string: str, *args: object) -> None:
        # 標準のアクセスログは上の一行形式と重複するため抑止する。
        return


def main() -> int:
    args = parse_arguments()
    release_dir = args.release_dir.expanduser().resolve()
    if not release_dir.is_dir():
        print(f"[mock_release] リリースディレクトリが見つからない: {release_dir}", file=sys.stderr)
        return 1
    if not 1 <= args.port <= MAX_PORT:
        print(f"[mock_release] ポート番号が不正: {args.port}", file=sys.stderr)
        return 1

    base_url = f"http://{args.host}:{args.port}/"
    try:
        release, asset_paths = build_release(release_dir, args.version, base_url)
    except (OSError, ValueError) as error:
        print(f"[mock_release] 初期化失敗: {error}", file=sys.stderr)
        return 1

    handler = type(
        "ConfiguredReleaseRequestHandler",
        (ReleaseRequestHandler,),
        {"release": release, "asset_paths": asset_paths},
    )
    server = ThreadingHTTPServer((args.host, args.port), handler)
    print(f"[mock_release] 起動: {base_url} version={args.version}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[mock_release] 停止", flush=True)
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
