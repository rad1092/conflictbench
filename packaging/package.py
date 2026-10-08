#!/usr/bin/env python3
"""Build relocatable desktop packages and smoke-test their extracted copies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

from prepare_sources import QT_VERSION, QT_SOURCE_NAME, QT_SOURCE_SHA256, sha256

ROOT = Path(__file__).resolve().parents[1]
# Every allowed Qt module belongs to the source archive published alongside the app.
QTBASE_MODULES = {"Core", "Gui", "Widgets", "Concurrent", "DBus", "OpenGL", "PrintSupport", "XcbQpa"}


def run(*args, **kwargs):
    args = [str(arg) for arg in args]
    print("+ " + " ".join(args), flush=True)
    return subprocess.run(args, check=True, **kwargs)


def copy_plugins(qt_plugins, destination, system):
    names = {
        "Darwin": {"platforms": ["qcocoa", "qoffscreen"], "styles": ["qmacstyle"]},
        "Windows": {"platforms": ["qwindows", "qoffscreen"], "styles": ["qmodernwindowsstyle"]},
        "Linux": {"platforms": ["qxcb", "qoffscreen"], "xcbglintegrations": ["qxcb-glx-integration", "qxcb-egl-integration"]},
    }[system]
    names["imageformats"] = ["qjpeg", "qgif", "qico"]
    result = []
    for group, plugins in names.items():
        for plugin in plugins:
            prefix, suffix = ("", ".dll") if system == "Windows" else ("lib", ".dylib" if system == "Darwin" else ".so")
            source = qt_plugins / group / f"{prefix}{plugin}{suffix}"
            if not source.is_file():
                if group in {"styles", "xcbglintegrations"}:
                    continue
                raise RuntimeError(f"Required plugin missing: {source}")
            target = destination / group / source.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            result.append(target)
    return result


def bundled_modules(stage):
    modules = set()
    for path in stage.rglob("*"):
        match = re.match(r"(?:lib)?Qt6?([A-Za-z0-9]+)(?:\.framework|\.so(?:\..*)?|\.dll)$", path.name)
        if match:
            modules.add(match.group(1))
    extra = modules - QTBASE_MODULES
    if extra:
        raise RuntimeError(f"Unexpected Qt modules without approved sources: {sorted(extra)}")
    if not {"Core", "Gui", "Widgets"} <= modules:
        raise RuntimeError(f"Missing runtime modules: {sorted(modules)}")
    return sorted(modules)


def clean_environment(qt_prefix):
    env = os.environ.copy()
    for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "QT_ROOT_DIR"):
        env.pop(key, None)
    env["PATH"] = os.pathsep.join(p for p in env.get("PATH", "").split(os.pathsep) if str(qt_prefix).lower() not in p.lower())
    return env


def smoke(executable, qt_prefix):
    environment = clean_environment(qt_prefix)
    with tempfile.TemporaryDirectory(prefix="conflictbench-package-smoke-") as directory:
        run(executable, "--smoke-test", env=environment, cwd=directory, timeout=60)


def package(args):
    system = platform.system()
    machine = {"AMD64": "x86_64", "aarch64": "arm64"}.get(platform.machine(), platform.machine())
    version_match = re.search(r"project\(ConflictBench\s+VERSION\s+(\d+\.\d+\.\d+)", (ROOT / "CMakeLists.txt").read_text())
    version = version_match.group(1)
    qt_bin = args.qt / "bin"
    qmake = qt_bin / ("qmake.exe" if system == "Windows" else "qmake")
    qt_version = subprocess.check_output([qmake, "-query", "QT_VERSION"], text=True).strip()
    if qt_version != QT_VERSION:
        raise RuntimeError(f"Expected Qt {QT_VERSION}, got {qt_version}")
    qt_plugins = Path(subprocess.check_output([qmake, "-query", "QT_INSTALL_PLUGINS"], text=True).strip())
    if not args.development and ("homebrew" in str(args.qt) or "Cellar" in str(args.qt)):
        raise RuntimeError("Use official Qt binaries for releases; Homebrew smoke builds require --development")
    if sha256(args.sources / QT_SOURCE_NAME) != QT_SOURCE_SHA256:
        raise RuntimeError("Corresponding Qt source is missing or changed")
    label = f"ConflictBench-{version}-{system.lower()}-{machine}" + ("-development" if args.development else "")
    args.output.mkdir(parents=True, exist_ok=True)
    work = ROOT / "package-work" / label
    if work.exists():
        shutil.rmtree(work)
    stage = work / label
    run("cmake", "--install", args.build, "--config", "Release", "--prefix", stage)
    share = stage / "share" / "conflictbench"
    share.mkdir(parents=True, exist_ok=True)
    shutil.copytree(ROOT / "packaging" / "licenses", share / "licenses", dirs_exist_ok=True)
    shutil.copytree(args.sources / "qt-notices", share / "licenses" / "qtbase", dirs_exist_ok=True)
    shutil.copy2(args.sources / "QT-SOURCE.json", share)
    if system == "Darwin":
        app = stage / "ConflictBench.app"
        resources = app / "Contents" / "Resources"
        resources.mkdir(parents=True, exist_ok=True)
        shutil.move(str(share), str(resources / "conflictbench"))
        share = resources / "conflictbench"
        shutil.rmtree(stage / "share")
        plugins = copy_plugins(qt_plugins, app / "Contents" / "PlugIns", system)
        run(qt_bin / "macdeployqt", app, "-no-plugins", "-no-codesign", *[f"-executable={plugin}" for plugin in plugins])
        (resources / "qt.conf").write_text("[Paths]\nPlugins = PlugIns\n", encoding="utf-8")
        # A local ad-hoc signature enables Apple Silicon loading; it is not a Developer ID signature.
        run("codesign", "--force", "--deep", "--sign", "-", app)
        run("codesign", "--verify", "--deep", "--strict", app)
        executable = app / "Contents" / "MacOS" / "ConflictBench"
        if not args.development:
            for path in app.rglob("*"):
                if path.is_file() and not path.is_symlink() and (path.suffix in {".dylib", ""}):
                    result = subprocess.run(["otool", "-L", str(path)], capture_output=True, text=True)
                    # otool prints the input's absolute path as a header, including
                    # for non-object resources. Only indented dependency records
                    # describe load paths that must be relocatable.
                    if result.returncode == 0 and re.search(r"^[ \t]+/(?:opt/homebrew|usr/local|Users|Applications/Qt)/", result.stdout, re.MULTILINE):
                        raise RuntimeError(f"Nonrelocatable dependency: {path}\n{result.stdout}")
    elif system == "Windows":
        executable = stage / "bin" / "ConflictBench.exe"
        plugins = copy_plugins(qt_plugins, stage / "bin", system)
        run(qt_bin / "windeployqt.exe", "--release", "--no-plugins", "--no-translations", "--no-compiler-runtime",
            "--skip-plugin-types", "generic,networkinformation,tls",
            "--no-system-d3d-compiler", "--no-system-dxc-compiler", "--no-opengl-sw", "--no-ffmpeg",
            "--dir", stage / "bin", executable, *plugins)
        vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        installation = Path(subprocess.check_output([vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip())
        redist = sorted((installation / "VC" / "Redist" / "MSVC").glob("*/x64/Microsoft.VC143.CRT"))[-1]
        permitted_dlls = {p.name.lower() for p in plugins}
        for runtime in redist.glob("*.dll"):
            shutil.copy2(runtime, stage / "bin" / runtime.name)
            permitted_dlls.add(runtime.name.lower())
        permitted_dlls.update(f"qt6{module.lower()}.dll" for module in QTBASE_MODULES)
        for path in stage.rglob("*.dll"):
            if path.name.lower() not in permitted_dlls:
                raise RuntimeError(f"Unreviewed Windows runtime dependency: {path.name}")
        if not (stage / "bin" / "vcruntime140.dll").exists():
            raise RuntimeError("Missing app-local Microsoft C++ runtime")
        (stage / "bin" / "qt.conf").write_text("[Paths]\nPlugins = .\n", encoding="utf-8")
    elif system == "Linux":
        executable = stage / "bin" / "ConflictBench"
        plugins = copy_plugins(qt_plugins, stage / "plugins", system)
        lib = stage / "lib"
        lib.mkdir()
        # Copy only Qtbase modules and ICU from the official Qt installation.
        for source in sorted((args.qt / "lib").glob("*.so*")):
            match = re.match(r"libQt6([A-Za-z0-9]+)\.so", source.name)
            if (match and match.group(1) in QTBASE_MODULES) or re.match(r"libicu(?:data|i18n|uc)\.so", source.name):
                shutil.copy2(source, lib / source.name, follow_symlinks=False)
        icu = list(lib.glob("libicuuc.so.*"))
        if not icu or any(not p.name.endswith(".73") and ".73." not in p.name for p in icu):
            raise RuntimeError("ICU version changed; review its notice before release")
        (stage / "bin" / "qt.conf").write_text("[Paths]\nPrefix = ..\nPlugins = plugins\nLibraries = lib\n", encoding="utf-8")
        # The loader uses this process-local path for Qt/ICU indirect dependencies.
        launcher = stage / "ConflictBench"
        launcher.write_text('#!/bin/sh\nset -eu\nappdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexport LD_LIBRARY_PATH="$appdir/lib"\nexec "$appdir/bin/ConflictBench" "$@"\n', encoding="utf-8")
        launcher.chmod(0o755)
        executable = launcher
        shutil.copy2(ROOT / "packaging" / "linux-runtime.txt", stage / "LINUX-REQUIREMENTS.txt")
        for binary in [stage / "bin" / "ConflictBench", *plugins]:
            env = clean_environment(args.qt)
            env["LD_LIBRARY_PATH"] = str(lib)
            output = subprocess.check_output(["ldd", str(binary)], env=env, text=True)
            if "not found" in output:
                raise RuntimeError(f"Missing dependencies: {binary}\n{output}")
    else:
        raise RuntimeError(f"Unsupported OS: {system}")
    modules = bundled_modules(stage)
    try:
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    except subprocess.CalledProcessError:
        commit = "uncommitted-development"
    manifest = {"application": "ConflictBench", "version": version, "commit": commit,
                "os": system, "architecture": machine, "qt_version": qt_version,
                "qt_modules": modules, "signed_by_publisher": False, "notarized": False,
                "development_only": args.development}
    (share / "BUILD-MANIFEST.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    # Resource changes invalidate ad-hoc signatures; sign the final bundle after inventory.
    if system == "Darwin":
        run("codesign", "--force", "--deep", "--sign", "-", stage / "ConflictBench.app")
        run("codesign", "--verify", "--deep", "--strict", stage / "ConflictBench.app")
    inventory = []
    for path in sorted(stage.rglob("*")):
        if path.is_file() and not path.is_symlink():
            inventory.append({"path": str(path.relative_to(stage)), "sha256": sha256(path), "bytes": path.stat().st_size})
    manifest["files"] = inventory
    if system == "Darwin":
        archive = args.output / (label + ".zip")
        run("ditto", "-c", "-k", "--keepParent", stage, archive)
    elif system == "Windows":
        archive = Path(shutil.make_archive(str(args.output / label), "zip", work, label))
    else:
        archive = args.output / (label + ".tar.gz")
        with tarfile.open(archive, "w:gz") as output:
            output.add(stage, arcname=label)
    # Check the actual archive after extraction, including paths containing spaces and Unicode.
    with tempfile.TemporaryDirectory(prefix="ConflictBench installed 한글 ") as temporary:
        extracted = Path(temporary)
        if system == "Darwin":
            run("ditto", "-x", "-k", archive, extracted)
        elif system == "Windows":
            with zipfile.ZipFile(archive) as package_zip:
                package_zip.extractall(extracted)
        else:
            with tarfile.open(archive) as package_tar:
                package_tar.extractall(extracted, filter="data")
        smoke(extracted / label / executable.relative_to(stage), args.qt)
    checksum = sha256(archive)
    archive.with_name(archive.name + ".sha256").write_text(f"{checksum}  {archive.name}\n", encoding="utf-8")
    (args.output / (label + "-manifest.json")).write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Archive smoke passed: {archive.name}  SHA256={checksum}")


if __name__ == "__main__":
    # Windows redirected consoles can default to a legacy code page. Keep
    # Unicode installation-path evidence printable without changing file paths.
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--qt", type=Path, required=True, help="The Qt architecture prefix, containing bin/qmake")
    parser.add_argument("--sources", type=Path, default=ROOT / "release-sources")
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--development", action="store_true", help="Allow Homebrew builds for local smoke testing; do not publish")
    package(parser.parse_args())
