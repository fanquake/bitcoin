#!/usr/bin/env python3
"""Generate a macOS SDK tarball from an Xcode .xip archive.
"""
import argparse
import io
import lzma
import os
import pathlib
import plistlib
import stat
import struct
import tarfile
import tempfile
import xml.etree.ElementTree as ET
import zlib

SDK_DIR = 'Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
SDK_SUBDIRS = ('usr/include', 'usr/lib', 'System/Library/Frameworks')
# Paths, relative to Xcode.app, to extract from the .xip.
WANTED = (
    'Contents/version.plist',
    f'{SDK_DIR}/System/Library/CoreServices/SystemVersion.plist',
    *(f'{SDK_DIR}/{d}' for d in SDK_SUBDIRS),
)


def pbzx_chunks(fp):
    """Yield the decompressed chunks of the pbzx "Content" file in a .xip (xar) archive."""
    magic, header_size, _version, toc_size = struct.unpack('>4sHHQ', fp.read(16))
    assert magic == b'xar!', "not a .xip archive"
    fp.seek(header_size)
    toc = ET.fromstring(zlib.decompress(fp.read(toc_size)))
    offset = next(int(f.findtext('data/offset')) for f in toc.iter('file') if f.findtext('name') == 'Content')
    fp.seek(header_size + toc_size + offset)
    magic, flags = struct.unpack('>4sQ', fp.read(12))
    assert magic == b'pbzx', "unexpected .xip content"
    while flags & (1 << 24):
        flags, size = struct.unpack('>QQ', fp.read(16))
        data = fp.read(size)
        # chunks are either xz-compressed or stored as-is
        yield lzma.decompress(data) if data.startswith(b'\xfd7zXZ\x00') else data


class ChunkStream(io.RawIOBase):
    """Read-only file object over an iterator of byte chunks."""
    def __init__(self, chunks):
        self.chunks = chunks
        self.buf = memoryview(b'')

    def readable(self):
        return True

    def readinto(self, b):
        while not self.buf:
            chunk = next(self.chunks, None)
            if chunk is None:
                return 0
            self.buf = memoryview(chunk)
        n = min(len(b), len(self.buf))
        b[:n] = self.buf[:n]
        self.buf = self.buf[n:]
        return n


def copy(src, size, dst=None):
    """Read size bytes from src, writing them to dst (if given)."""
    while size:
        data = src.read(min(size, 1 << 20))
        if not data:
            raise EOFError("truncated .xip archive")
        if dst:
            dst.write(data)
        size -= len(data)


def extract(xip, dest):
    """Extract the WANTED parts of the Xcode.app in xip to dest.

    The archive is an odc cpio stream. Only the first entry of a hardlinked file
    carries its contents. Later entries for the same file carry a placeholder,
    and must be linked to the first. As the first entry is often outside of the
    macOS SDK, every hardlinked file is kept, under dest/hardlinks.
    """
    hardlinks = {}
    with xip.open('rb') as fp:
        stream = io.BufferedReader(ChunkStream(pbzx_chunks(fp)), 1 << 20)
        while True:
            header = stream.read(76)
            assert header[:6] == b'070707', "unexpected cpio format"
            key = header[6:18]  # dev + ino
            mode = int(header[18:24], 8)
            nlink = int(header[36:42], 8)
            name = stream.read(int(header[59:65], 8)).rstrip(b'\0').decode()
            size = int(header[65:76], 8)
            if name == 'TRAILER!!!':
                break

            app, _, rel = name.removeprefix('./').partition('/')
            path = None
            if any(rel == w or rel.startswith(w + '/') for w in WANTED):
                path = dest / app / rel
                path.parent.mkdir(parents=True, exist_ok=True)

            if stat.S_ISREG(mode) and nlink > 1:
                if key in hardlinks:
                    copy(stream, size)
                    if path:
                        os.link(hardlinks[key], path)
                    continue
                path = path or dest / 'hardlinks' / str(len(hardlinks))
                path.parent.mkdir(exist_ok=True)
                hardlinks[key] = path

            if path is None:
                copy(stream, size)
            elif stat.S_ISDIR(mode):
                path.mkdir(exist_ok=True)
            elif stat.S_ISLNK(mode):
                path.symlink_to(stream.read(size).decode())
            else:
                with path.open('wb') as f:
                    copy(stream, size, f)
                path.chmod(stat.S_IMODE(mode))


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('xcode_xip', metavar='XCODEXIP', type=pathlib.Path)
    parser.add_argument('-o', metavar='OUTSDKTAR', dest='out_sdkt', type=pathlib.Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        extract(args.xcode_xip, tmp)
        xcode_app = next(tmp.glob('*.app'))
        sdk_dir = xcode_app / SDK_DIR

        xcode = plistlib.loads((xcode_app / 'Contents/version.plist').read_bytes())
        sdk = plistlib.loads((sdk_dir / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
        print(f"Found Xcode {xcode['CFBundleShortVersionString']} ({xcode['ProductBuildVersion']}), "
              f"MacOSX SDK {sdk['ProductVersion']} ({sdk['ProductBuildVersion']})")

        out_name = f"Xcode-{xcode['CFBundleShortVersionString']}-{xcode['ProductBuildVersion']}-extracted-SDK-with-libcxx-headers"
        out_sdkt = args.out_sdkt or pathlib.Path(f'{out_name}.tar')

        def normalize(tarinfo):
            """Drop Swift/module files and make metadata deterministic."""
            if tarinfo.name.endswith(('.swiftmodule', '.modulemap')):
                return None
            # Kept for compatibility with previously generated SDK tarballs.
            if tarinfo.linkname.startswith('./'):
                tarinfo.linkname = f'{out_name}/{tarinfo.linkname[2:]}'
            tarinfo.mtime = 0
            tarinfo.uid = tarinfo.gid = 0
            tarinfo.uname = tarinfo.gname = ''
            tarinfo.mode = 0o755 if tarinfo.mode & 0o100 else 0o644
            return tarinfo

        with tarfile.open(out_sdkt, 'w', format=tarfile.PAX_FORMAT) as tar:
            # recursion adds entries in sorted order
            for d in SDK_SUBDIRS:
                tar.add(sdk_dir / d, arcname=f'{out_name}/{d}', filter=normalize)
    print(out_sdkt.resolve())


if __name__ == '__main__':
    run()
