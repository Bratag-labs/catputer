#!/usr/bin/env python3
"""Build a Debian package for the Cardputer Zero (APPLaunch integration).

Packaged files:
  usr/local/bin/lofi_native                      the player binary
  usr/share/APPLaunch/applications/catputer.desktop.in
                                                 launcher template; postinst
                                                 expands @STATE@ to the
                                                 installing user's
                                                 ~/.local/state.bin
  usr/share/APPLaunch/share/images/catputer.png  launcher icon

Maintainer scripts:
  preinst  stops a running lofi_native before the binary is replaced
  postinst writes catputer.desktop from the template with the correct home
  postrm   kills a running instance and removes the generated .desktop

Usage (on the device, after building lofi_native):
  python3 tools/build_deb.py
  sudo dpkg -i build/deb/catputer_<version>_arm64.deb

The packaged binary's ELF machine is checked against --arch, so an x86-64
binary can never land in an arm64 package.
"""

import argparse
import gzip
import io
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

PKG = "catputer"
BIN_NAME = "lofi_native"

DESKTOP_TEMPLATE = """\
[Desktop Entry]
Type=Application
Name=Catputer
Exec=/usr/local/bin/lofi_native --state @STATE@
Icon=share/images/catputer.png
"""

POSTINST = """\
#!/bin/sh
set -e
root="${DPKG_ROOT:-}"
in="$root/usr/share/APPLaunch/applications/catputer.desktop.in"
out="$root/usr/share/APPLaunch/applications/catputer.desktop"
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
  home=$(getent passwd "$SUDO_USER" | cut -d: -f6)
fi
if [ -z "${home:-}" ]; then
  home=$(getent passwd | awk -F: '$3 >= 1000 && $3 < 65534 { print $6; exit }')
fi
if [ -z "$home" ]; then
  echo "catputer: cannot determine a user home for the state file" >&2
  exit 1
fi
sed "s|@STATE@|$home/.local/state.bin|" "$in" > "$out"
chmod 644 "$out"
"""

PREINST = """\
#!/bin/sh
# Stop a running instance before replacing the binary.
pkill -x lofi_native 2>/dev/null || true
"""

POSTRM = """\
#!/bin/sh
# $1: remove | upgrade | failed-upgrade | deconfigure
root="${DPKG_ROOT:-}"
case "$1" in
  remove|deconfigure)
    pkill -x lofi_native 2>/dev/null || true
    rm -f "$root/usr/share/APPLaunch/applications/catputer.desktop"
    ;;
esac
"""

EM = {0x3E: "amd64", 0xB7: "arm64"}  # EM_X86_64, EM_AARCH64


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--binary", default="build/cmake/lofi_native")
    p.add_argument("--icon", default="assets/app-icon.png")
    p.add_argument("--version-file", default="VERSION")
    p.add_argument("--arch", default="arm64", choices=["arm64", "amd64"])
    p.add_argument("--maintainer", default="bratag <bratag@gmail.com>")
    p.add_argument("--output", default=None,
                   help="output .deb path (default build/deb/<pkg>_<ver>_<arch>.deb)")
    p.add_argument("--no-strip", action="store_true",
                   help="keep the packaged binary unstripped")
    return p.parse_args()


def check_elf_arch(path, want):
    with open(path, "rb") as f:
        hdr = f.read(20)
    if hdr[:4] != b"\x7fELF":
        sys.exit(f"error: {path} is not an ELF file")
    machine = struct.unpack_from("<H", hdr, 18)[0]
    got = EM.get(machine)
    if got != want:
        sys.exit(f"error: binary is {got or hex(machine)} but --arch is {want}")


def stage_tree(root, binary, icon, version, arch, maintainer, strip):
    for rel in [
        "usr/local/bin",
        "usr/share/APPLaunch/applications",
        "usr/share/APPLaunch/share/images",
        "DEBIAN",
    ]:
        os.makedirs(os.path.join(root, rel), exist_ok=True)

    shutil.copyfile(binary, os.path.join(root, f"usr/local/bin/{BIN_NAME}"))
    if strip and shutil.which("strip"):
        subprocess.run(["strip", os.path.join(root, f"usr/local/bin/{BIN_NAME}")],
                       check=True)
    elif strip:
        print("note: 'strip' not found; keeping binary unstripped", file=sys.stderr)

    shutil.copyfile(icon, os.path.join(root, "usr/share/APPLaunch/share/images",
                                       "catputer.png"))
    with open(os.path.join(root, "usr/share/APPLaunch/applications",
                           "catputer.desktop.in"), "w") as f:
        f.write(DESKTOP_TEMPLATE)

    size = sum((os.path.getsize(os.path.join(root, r)) + 1023) // 1024
               for r in ["usr/local/bin/%s" % BIN_NAME,
                         "usr/share/APPLaunch/share/images/catputer.png",
                         "usr/share/APPLaunch/applications/catputer.desktop.in"])
    control = (
        f"Package: {PKG}\n"
        f"Version: {version}\n"
        f"Architecture: {arch}\n"
        f"Maintainer: {maintainer}\n"
        f"Installed-Size: {size}\n"
        "Section: sound\n"
        "Priority: optional\n"
        "Description: Endless lofi radio for M5Stack Cardputer\n"
        "  Offline seeded lofi music with a pixel-art cat scene, built for\n"
        "  the Cardputer Zero (Linux, arm64). Installs the native player to\n"
        f"  /usr/local/bin/{BIN_NAME} and registers it with APPLaunch.\n"
    )
    with open(os.path.join(root, "DEBIAN/control"), "w") as f:
        f.write(control)
    for name, body in [("postinst", POSTINST), ("preinst", PREINST),
                       ("postrm", POSTRM)]:
        p = os.path.join(root, "DEBIAN", name)
        with open(p, "w") as f:
            f.write(body)
        os.chmod(p, 0o755)


def make_tarball(root, members, exec_names=()):
    """data.tar / control.tar as dpkg expects: ./-prefixed paths, root, mtime 0."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for arcname in members:
            path = os.path.join(root, arcname.lstrip("./"))
            ti = tarfile.TarInfo(arcname)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            ti.mtime = 0
            if os.path.isdir(path):
                ti.type = tarfile.DIRTYPE
                ti.mode = 0o755
                tar.addfile(ti)
            else:
                ti.type = tarfile.REGTYPE
                ti.size = os.path.getsize(path)
                ti.mode = 0o755 if arcname in exec_names else 0o644
                with open(path, "rb") as f:
                    tar.addfile(ti, f)
    return buf.getvalue()


def ar_write(path, members):
    """Minimal GNU ar writer (dpkg-compatible: uid/gid 0, mtime 0)."""
    out = io.BytesIO()
    out.write(b"!<arch>\n")
    for name, data in members:
        # ar header: name/16, mtime/12, uid/6, gid/6, mode/8, size/10, mag/2
        hdr = (
            f"{name[:16]:<16}"
            f"0{' ' * 11}"
            f"0{' ' * 5}"
            f"0{' ' * 5}"
            f"100644{' ' * 2}"
            f"{len(data):>10}"
            f"`\n"
        )
        if len(hdr) != 60:
            raise RuntimeError(f"ar header is {len(hdr)} bytes, want 60")
        out.write(hdr.encode("ascii"))
        out.write(data)
        if len(data) % 2:
            out.write(b"\n")
    with open(path, "wb") as f:
        f.write(out.getvalue())


def main():
    args = parse_args()
    binary = os.path.abspath(args.binary)
    icon = os.path.abspath(args.icon)
    for p in (binary, icon):
        if not os.path.isfile(p):
            sys.exit(f"error: missing input: {p}")
    with open(os.path.abspath(args.version_file)) as f:
        version = f.readline().strip()
    check_elf_arch(binary, args.arch)

    out = args.output or os.path.join("build/deb", f"{PKG}_{version}_{args.arch}.deb")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.join(tmp, "pkg")
        os.makedirs(root)
        stage_tree(root, binary, icon, version, args.arch, args.maintainer,
                   not args.no_strip)

        dpkg_deb = shutil.which("dpkg-deb")
        if dpkg_deb:
            subprocess.run([dpkg_deb, "--build", "--root-owner-group",
                            root, out], check=True)
        else:
            print("note: dpkg-deb not found; assembling with built-in ar/tar",
                  file=sys.stderr)
            debian = os.path.join(root, "DEBIAN")
            data_members = ["./"]
            for dirpath, dirnames, filenames in os.walk(os.path.join(root, "usr")):
                for n in sorted(dirnames):
                    data_members.append("./" + os.path.relpath(
                        os.path.join(dirpath, n), root))
                for n in sorted(filenames):
                    data_members.append("./" + os.path.relpath(
                        os.path.join(dirpath, n), root))
            ar_write(out, [
                ("debian-binary/", b"2.0\n"),
                ("control.tar.gz/", make_tarball(
                    debian,
                    [f"./{e}" for e in sorted(os.listdir(debian))],
                    exec_names={"./postinst", "./preinst", "./postrm"})),
                ("data.tar.gz/", make_tarball(
                    root, data_members,
                    exec_names={f"./usr/local/bin/{BIN_NAME}"})),
            ])

    # Self-check: the archive must round-trip through ar/tar readers.
    with open(out, "rb") as f:
        magic = f.read(8)
    if magic != b"!<arch>\n":
        sys.exit(f"error: {out} is not an ar archive")
    if dpkg_deb:
        subprocess.run([dpkg_deb, "-c", out], check=True)
        subprocess.run([dpkg_deb, "-I", out], check=True)
    else:
        print(f"built {out}")
        print("  (verify on the device with: dpkg-deb -I/-c or dpkg -i --dry-run)")


if __name__ == "__main__":
    main()
