#!/usr/bin/env python3
"""Build LofiFxPlanarStabilizer OFX plugin and install.

    macOS:   /Library/OFX/Plugins
    Linux:   /usr/OFX/Plugins
    Windows: %CommonProgramFiles%\\OFX\\Plugins
"""

import os, shutil, subprocess, sys

SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR   = os.path.join(SCRIPT_DIR, "build")
BUNDLE_NAME = "LofiFxPlanarStabilizer.ofx.bundle"
BUNDLE_SRC  = os.path.join(BUILD_DIR, BUNDLE_NAME)

def install_dir() -> str:
    if sys.platform == "darwin":
        return "/Library/OFX/Plugins"
    if sys.platform.startswith("win"):
        common = os.environ.get("CommonProgramFiles", r"C:\Program Files\Common Files")
        return os.path.join(common, "OFX", "Plugins")
    return "/usr/OFX/Plugins"

def run(cmd, **kwargs):
    print(">>>", " ".join(cmd))
    subprocess.check_call(cmd, **kwargs)

def adhoc_sign(bundle_path: str):
    if sys.platform != "darwin":
        return
    if not shutil.which("codesign"):
        print("codesign not found — skipping ad-hoc signing")
        return
    print("\n=== Ad-hoc signing bundle ===")
    run(["codesign", "--sign", "-", "--force", "--deep", bundle_path])

def strip_quarantine(bundle_path: str):
    if sys.platform != "darwin":
        return
    if not shutil.which("xattr"):
        return
    print("\n=== Removing quarantine attribute ===")
    run(["sudo", "xattr", "-r", "-d", "com.apple.quarantine", bundle_path])

def copy_bundle(src: str, dst: str):
    if os.path.exists(dst):
        run(["sudo", "rm", "-rf", dst])
    run(["sudo", "cp", "-R", src, dst])

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    print("\n=== Configuring ===")
    cfg = ["cmake", ".."]
    if sys.platform == "darwin":
        cfg += ["-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64",
                "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0",
                "-DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"]
    elif sys.platform.startswith("win"):
        cfg += ["-A", "x64"]
    run(cfg, cwd=BUILD_DIR)
    print("\n=== Building ===")
    build = ["cmake", "--build", "."]
    if sys.platform.startswith("win"):
        build += ["--config", "Release"]
    run(build, cwd=BUILD_DIR)
    adhoc_sign(BUNDLE_SRC)
    dst = os.path.join(install_dir(), BUNDLE_NAME)
    print(f"\n=== Installing to {dst} ===")
    copy_bundle(BUNDLE_SRC, dst)
    strip_quarantine(dst)
    print("\nDone. Restart DaVinci Resolve to load the updated plugin.")

if __name__ == "__main__":
    main()
