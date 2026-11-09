"""Write the empty v12 pak used to trigger mounting a same-name IoStore container.

This writes no assets, performs no cooking, and does not load anything in a game.
See docs/PAK_WORKFLOW.md for the required .utoc/.ucas and DLL integration.
"""
import argparse
import hashlib
import struct
from pathlib import Path


def mount_pak_bytes():
    """The same 313-byte layout as the project's verified supplement container."""
    def fstring(text):
        encoded = text.encode('ascii') + b'\0'
        return struct.pack('<i', len(encoded)) + encoded

    directory = struct.pack('<i', 1) + fstring('/') + struct.pack('<i', 0)
    primary_wo = fstring('../../../') + struct.pack('<iQ', 0, 0) + struct.pack('<I', 0)
    primary_len = len(primary_wo) + 4 + 8 + 8 + 20 + 4 + 4
    primary = primary_wo + struct.pack('<Iqq', 1, primary_len, len(directory)) + hashlib.sha1(directory).digest()
    primary += struct.pack('<i', 0) + struct.pack('<i', 0)
    assert len(primary) == primary_len
    footer = (bytes(16) + b'\0' + struct.pack('<Iiqq', 0x5A6F12E1, 12, 0, len(primary))
              + hashlib.sha1(primary).digest() + bytes(5 * 32))
    assert len(footer) == 221
    return primary + directory + footer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='output .pak, with the same stem as your .utoc/.ucas')
    args = parser.parse_args()
    if args.output.suffix.lower() != '.pak':
        parser.error('output must end with .pak')
    data = mount_pak_bytes()
    # Exclusive creation prevents replacing an existing asset-bearing pak.
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as file:
        file.write(data)
    print(f'{args.output}: {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
