"""Build/archive one explicitly named lamp role; optional flash only from that role's manifest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ROLES = {
    "edge-runtime": ("apple-lamp-ov3660-edge-runtime", b"Firmware role: EDGE-RUNTIME"),
    "omni-poc": ("apple-lamp-ov3660-omni-poc", b"Firmware role: OMNI-POC"),
    "simulator": ("apple-lamp-ov3660", b"Firmware role: SIMULATOR-BRIDGE"),
    "local-readonly": ("apple-lamp-ov3660-local-readonly", b"Firmware role: LOCAL-READONLY"),
    "local-hardware": ("apple-lamp-ov3660-local-hardware", b"Firmware role: LOCAL-HARDWARE"),
    "local-twin": ("apple-lamp-ov3660-local-twin", b"Firmware role: LOCAL-TWIN"),
}


def validate_role(image: bytes, role: str, sdk: str):
    name, marker = ROLES[role]
    if marker not in image or name.encode() not in image:
        raise ValueError("Binary role/board identity does not match requested artifact")
    for other, (_, other_marker) in ROLES.items():
        if other != role and other_marker in image:
            raise ValueError("Binary contains the opposite application role")
    if ("CONFIG_APPLE_LAMP_OMNI_REALTIME=y" in sdk.splitlines()) != (role in ("omni-poc", "edge-runtime")):
        raise ValueError("Omni protocol configuration does not match role")
    if ("CONFIG_APPLE_LAMP_EDGE_RUNTIME=y" in sdk.splitlines()) != (role == "edge-runtime"):
        raise ValueError("Edge runtime configuration does not match role")
    expected_local = role != "simulator"
    for symbol in ("APPLE_LAMP_LOCAL_EXECUTOR", "APPLE_LAMP_UART_PROBE", "COMPILER_CXX_EXCEPTIONS"):
        if (f"CONFIG_{symbol}=y" in sdk.splitlines()) != expected_local:
            raise ValueError(f"Active configuration does not match role: {symbol}")
    if ("CONFIG_APPLE_LAMP_HARDWARE_MOTION=y" in sdk.splitlines()) != (role in ("local-hardware", "local-twin", "omni-poc", "edge-runtime")):
        raise ValueError("Hardware motion configuration does not match role")
    if ("CONFIG_APPLE_LAMP_TWIN_WEB=y" in sdk.splitlines()) != (role in ("local-twin", "omni-poc", "edge-runtime")):
        raise ValueError("Web test panel configuration does not match role")



def archive(role: str):
    build = ROOT / "build"
    image = (build / "xiaozhi.bin").read_bytes()
    validate_role(image, role, (ROOT / "sdkconfig").read_text(encoding="utf-8"))
    args = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    digest = hashlib.sha256(image).hexdigest()
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    output = ROOT / "artifacts/lamp" / role / (commit[:8] + "-" + digest[:12])
    files = []
    sources = []
    for address, relative in args["flash_files"].items():
        source = (build / relative).resolve()
        if not source.is_relative_to(build.resolve()) or not source.is_file():
            raise ValueError("Unexpected flash image path")
        target = (output / relative).resolve()
        if not target.is_relative_to(output.resolve()):
            raise ValueError("Unexpected archive image path")
        sources.append((source, target))
        files.append({"address": address, "file": relative, "sha256": hashlib.sha256(source.read_bytes()).hexdigest()})
    manifest = {"role": role, "board_name": ROLES[role][0], "git_commit": commit,
                "worktree_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
                "app_sha256": digest, "motion_enabled": role in ("local-hardware", "local-twin", "omni-poc", "edge-runtime"),
                "startup_armed": False,
                "uart_probe_enabled": role != "simulator", "flash_files": files,
                "write_flash_args": args["write_flash_args"], "contains_local_configuration": True}
    if role in ("omni-poc", "edge-runtime"):
        manifest["cloud_motion_enabled"] = role == "edge-runtime"
    manifest_path = output / "manifest.json"
    if manifest_path.exists() and json.loads(manifest_path.read_text(encoding="utf-8")) != manifest:
        raise ValueError("An immutable build record already exists with different metadata")
    if manifest_path.exists():
        for item in files:
            target = output / item["file"]
            if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != item["sha256"]:
                raise ValueError("Existing archived image checksum mismatch; refusing to repair in place")
        return manifest_path
    output.mkdir(parents=True, exist_ok=True)
    for source, target in sources:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    return manifest_path


def flash(manifest_path: Path, role: str, port: str):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest["role"] != role:
        raise ValueError("Refusing to flash another firmware role")
    files = []
    for item in manifest["flash_files"]:
        image = (manifest_path.parent / item["file"]).resolve()
        if not image.is_relative_to(manifest_path.parent.resolve()):
            raise ValueError("Flash image escaped its role archive")
        if hashlib.sha256(image.read_bytes()).hexdigest() != item["sha256"]:
            raise ValueError("Archived flash image checksum mismatch")
        files += [item["address"], str(image)]
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
                    "--before", "default-reset", "--after", "hard-reset", "write-flash",
                    *manifest["write_flash_args"], *files], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--role", choices=ROLES, required=True)
    parser.add_argument("--archive-only", action="store_true", help="Validate/archive existing build; does not guess role")
    parser.add_argument("--flash", action="store_true")
    parser.add_argument("--port")
    args = parser.parse_args()
    if args.flash and not args.port:
        parser.error("--flash requires explicit --port")
    if not args.archive_only:
        env = os.environ.copy(); env["CCACHE_DISABLE"] = "1"; env["CMAKE_NINJA_FORCE_RESPONSE_FILE"] = "1"
        subprocess.run([sys.executable, "scripts/build.py", "waveshare/apple-lamp-ov3660", "--name",
                        ROLES[args.role][0], "--language", "zh-CN"], cwd=ROOT, env=env, check=True)
    manifest = archive(args.role)
    print(f"ROLE={args.role}; manifest={manifest}; hardware motion capability={args.role in ('local-hardware', 'local-twin', 'omni-poc', 'edge-runtime')}; startup disarmed", flush=True)
    if args.flash:
        flash(manifest, args.role, args.port)


if __name__ == "__main__":
    main()
