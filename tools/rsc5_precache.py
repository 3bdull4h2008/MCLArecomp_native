#!/usr/bin/env python3
"""Pre-translate every RSC5 resource a soak measured, into ready files.

WHY (2026-10-31). Since the non-UI RSC5 decode was admitted
(RSC5-DECODE-NONUI), a boot serves ~2,845 members instead of ~1,216, and the
guest's 512 MiB physical pool goes FULL while the title screen still waits.
Decoding those members at serve time also re-does work on every attempt. This
tool moves the translate step OFF the boot path: it walks the measured member
table out of a soak log, decodes each container with the project's own LZX
frame-chain rule, verifies the decode reproduces the container's declared
length, and writes the decoded bodies plus an index the host can serve from.

INPUT is a soak log, not the archive TOC: the on-disk RPF TOC is AES and the
cleartext hash-sorted table only exists in guest memory (tools/rpf_offline.py
header/names), so the WEASURED (path, serveOff, declared) rows in a log are the
only exact member list available offline. A member absent from the log is
absent from this cache -- by construction, the log is the census.

OUTPUT  <out>/index.tsv   path, serveOff, declared, produced, head, status
        <out>/blobs/<sanitised path>.bin

USAGE
  py -3 tools/rsc5_precache.py build/rsc5a.log
  py -3 tools/rsc5_precache.py build/rsc5a.log --out build/asset_cache
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(ROOT, '.research', '360tools', 'tools'))
sys.path.insert(0, _HERE)

from lzx_decompress import LZXDecoder          # noqa: E402
import xmem_decode as X                        # noqa: E402


def parse_members(log_path):
    """(path -> (off, declared)) for every RSC5 row the log measured.

    Both the decode rows and ASSET-CACHE-HIT rows are read: a boot that served
    a member from the cache does not emit a decode row, so reading only the
    decode rows would shrink the table on every warm run.

    `.xspm` needs its own arm. Its TOC word[2] is NOT an offset: for all 25
    `.xspm` members the vehicle TOC holds, word[2] is (containerBase | 0x013),
    so the decode rows for that family either do not exist (before the
    XSPM-REANCHOR fix the member was refused) or point 19 bytes into the
    container. The arm below takes the TOC row's four words and accepts the
    page base ONLY when the container agrees with the record on three
    independent facts -- RSC5 magic, the record's own word[3] at +0x08, and
    declared length + 20 == the record's own word[1]. That is a proof, not a
    mask-and-hope, so a member that fails any of the three is simply left out.
    """
    row = re.compile(r"(?:RSC5-(?:DECODE-NONUI|XMEM-DECODE(?:-FAIL)?)|"
                     r"ASSET-CACHE-HIT) "
                     r"path='([^']+)' off=([0-9A-Fa-f]{8}) "
                     r"declared=(\d+)")
    out = {}
    with open(log_path, 'rb') as f:
        for line in f:
            m = row.search(line.decode('utf-8', 'replace'))
            if not m:
                continue
            path, off, declared = m.groups()
            out[path] = (int(off, 16), int(declared))
    return out


def parse_xspm(log_path, archive):
    """path -> (containerBase, declared) for `.xspm`, proved from the TOC row."""
    toc = re.compile(r"toc=\[([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8}) "
                     r"([0-9A-F]{8})\]")
    want = {}
    with open(log_path, 'rb') as f:
        for line in f:
            s = line.decode('utf-8', 'replace')
            if '.xspm' not in s:
                continue
            mp = re.search(r"path='([^']*\.xspm)'", s)
            mt = toc.search(s)
            if mp and mt:
                want[mp.group(1)] = [int(x, 16) for x in mt.groups()]
    good, packed = {}, 0
    for path, w in sorted(want.items()):
        base = w[2] & 0xFFFFF000
        if base == 0:
            continue
        try:
            with open(archive, 'rb') as f:
                f.seek(base)
                b = f.read(20)
        except OSError:
            continue
        if len(b) < 20:
            continue
        if X.be32(b, 0) != X.RSC5 or X.be32(b, 12) != X.XCOMP:
            continue
        declared = X.be32(b, 16)
        if X.be32(b, 8) != w[3] or declared + 20 != w[1] or declared == 0:
            continue
        good[path] = (base, declared)
    return good



def parse_index(out_dir):
    """path -> (off, declared) from an index.tsv this tool wrote before.

    WHY (measured 2026-10-31): the tool's ONLY census was a soak log, and a log
    from a WARM cache no longer carries the decode rows -- `build/cache1.log`
    (all members served from disk) holds 18, where `build/rsc5a.log` (cold) holds
    2,845. Re-running against the warm log therefore tried to replace a
    2,845-member index with 18, i.e. it would have DELETED the ready files map
    that makes the warm boot possible. The index is read back and unioned with
    whatever the log proves, so a re-run can only ever add.
    """
    path = os.path.join(out_dir, 'index.tsv')
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) < 3 or parts[0] == 'path':
                continue
            try:
                out[parts[0]] = (int(parts[1], 16), int(parts[2]))
            except ValueError:
                continue
    return out


def sanitise(path):
    return re.sub(r'[^A-Za-z0-9._-]+', '__', path)


def decode(archive, off, declared):
    with open(archive, 'rb') as f:
        f.seek(off)
        pkg = f.read(20 + declared + 64)
    if len(pkg) < 20 + 16 or X.be32(pkg, 0) != X.RSC5:
        return None, 'not-RSC5'
    if X.be32(pkg, 12) != X.XCOMP:
        return None, 'not-XComp'
    unc = X.be32(pkg, 16)
    if unc != declared:
        return None, 'declared-mismatch:%d' % unc
    frames = X.chain(pkg)
    body, used = X.decode_chain(pkg, unc, frames)
    if len(body) != unc:
        return None, 'short:%d/%d' % (len(body), unc)
    return body, 'ok:%d/%d' % (used, len(frames))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--archive', default=os.path.join(ROOT, 'build', 'game_data', 'xarchive_cache.rpf'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'asset_cache'))
    ap.add_argument('--force', action='store_true',
                    help='re-decode every member instead of reusing a blob '
                         'whose length already matches the container')
    args = ap.parse_args()

    prior = parse_index(args.out)
    members = parse_members(args.log)
    print('members in log: %d' % len(members))
    xspm = parse_xspm(args.log, args.archive)
    added = 0
    for p, v in xspm.items():
        if p not in members:
            members[p] = v
            added += 1
    print('.xspm proved from the TOC row: %d (%d not previously in the table)'
          % (len(xspm), added))
    kept = 0
    for p, v in prior.items():
        if p not in members:
            members[p] = v
            kept += 1
    print('members kept from the existing index: %d (total %d)'
          % (kept, len(members)))
    blobdir = os.path.join(args.out, 'blobs')
    os.makedirs(blobdir, exist_ok=True)

    rows, bad, total = [], [], 0
    for i, (path, (off, declared)) in enumerate(sorted(members.items())):
        blob = os.path.join(blobdir, sanitise(path) + '.bin')
        # Incremental: a blob whose size already equals the container's own
        # declared length is the body (the bootstrap verifies that on load).
        if not args.force and os.path.isfile(blob) and \
                os.path.getsize(blob) == declared:
            head = open(blob, 'rb').read(4)
            rows.append((path, off, declared, declared, '%08X' % X.be32(head, 0),
                         'cached'))
            total += declared
            continue
        body, status = decode(args.archive, off, declared)
        if body is None:
            bad.append((path, off, declared, status))
            rows.append((path, off, declared, 0, '00000000', status))
            continue
        name = sanitise(path) + '.bin'
        with open(os.path.join(blobdir, name), 'wb') as g:
            g.write(body)
        total += len(body)
        rows.append((path, off, declared, len(body), '%08X' % X.be32(body, 0), status))
        if (i + 1) % 250 == 0:
            print('  %d/%d  %d bytes' % (i + 1, len(members), total))

    with open(os.path.join(args.out, 'index.tsv'), 'w', encoding='utf-8') as g:
        g.write('path\toff\tdeclared\tproduced\thead\tstatus\n')
        for r in rows:
            g.write('%s\t%08X\t%d\t%d\t%s\t%s\n' % r)

    print('translated %d/%d members, %d bytes' % (len(rows) - len(bad), len(rows), total))
    if bad:
        print('NOT TRANSLATED (%d):' % len(bad))
        for p, o, d, s in bad[:40]:
            print('  %08X %8d %-16s %s' % (o, d, s, p))


if __name__ == '__main__':
    main()
