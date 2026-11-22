#!/usr/bin/env python3
"""
opengate_mp4_tool.py — Zero-dependency Open Gate 3:2 MP4/MOV inspector & lossless aspect fixer.

Designed for Sony A7S III (a7s3-plus) Open Gate 3:2 recordings:
  - Inspects coded resolution (stsd), container display dimensions (tkhd), pasp box,
    and verifies whether Sony's Real-Time Metadata (rtmd / meta gyro + lens track)
    survived for Catalyst Prepare / Gyroflow.
  - Patches video track `tkhd` display height to exact 3:2 (e.g. 3840x2560 or 4096x2731)
    with ZERO re-encoding (instant in-place or stream-copy) while keeping audio, timecode,
    and Sony gyro metadata 100% untouched.
  - Generates ready-to-run 10-bit FFmpeg square-pixel (3840x2560, SAR 1:1) export commands.
"""
import argparse
import os
import shutil
import struct
import sys
from pathlib import Path


CONTAINER_TYPES = {
    b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts', b'dinf', b'udta',
}
VISUAL_SAMPLE_ENTRIES = {b'hvc1', b'hev1', b'avc1', b'avc3', b'apch', b'apcn', b'apcs', b'apco', b'ap4h'}


def iter_boxes(fh, start: int, end: int):
    """Yield (box_type, box_start, header_len, box_end) within [start, end)."""
    pos = start
    while pos + 8 <= end:
        fh.seek(pos)
        hdr = fh.read(8)
        if len(hdr) < 8:
            break
        size32, btype = struct.unpack('>I4s', hdr)
        hdr_len = 8
        if size32 == 1:
            ext = fh.read(8)
            if len(ext) < 8:
                break
            box_size = struct.unpack('>Q', ext)[0]
            hdr_len = 16
        elif size32 == 0:
            box_size = end - pos
        else:
            box_size = size32
        if box_size < hdr_len or pos + box_size > end:
            break
        yield btype, pos, hdr_len, pos + box_size
        pos += box_size


def parse_track(fh, trak_start: int, trak_hdr: int, trak_end: int) -> dict:
    info = {
        'handler': None,
        'codec': None,
        'coded_w': None,
        'coded_h': None,
        'pasp': None,
        'tkhd_pos': None,
        'tkhd_version': None,
        'tkhd_w_offset': None,
        'tkhd_h_offset': None,
        'tkhd_w': None,
        'tkhd_h': None,
        'sample_count': None,
    }
    for btype, bstart, bhdr, bend in iter_boxes(fh, trak_start + trak_hdr, trak_end):
        pstart = bstart + bhdr
        if btype == b'tkhd':
            fh.seek(pstart)
            ver = fh.read(1)[0]
            w_off = pstart + (88 if ver == 1 else 76)
            h_off = w_off + 4
            if h_off + 4 <= bend:
                fh.seek(w_off)
                w_fp, h_fp = struct.unpack('>II', fh.read(8))
                info['tkhd_pos'] = bstart
                info['tkhd_version'] = ver
                info['tkhd_w_offset'] = w_off
                info['tkhd_h_offset'] = h_off
                info['tkhd_w'] = w_fp / 65536.0
                info['tkhd_h'] = h_fp / 65536.0
        elif btype == b'mdia':
            for mtype, mstart, mhdr, mend in iter_boxes(fh, pstart, bend):
                mpstart = mstart + mhdr
                if mtype == b'hdlr' and mpstart + 12 <= mend:
                    fh.seek(mpstart + 8)
                    info['handler'] = fh.read(4).decode('latin1', 'replace')
                elif mtype == b'minf':
                    for ntype, nstart, nhdr, nend in iter_boxes(fh, mpstart, mend):
                        if ntype == b'stbl':
                            for stype, sstart, shdr, send in iter_boxes(fh, nstart + nhdr, nend):
                                spstart = sstart + shdr
                                if stype == b'stsz' and spstart + 12 <= send:
                                    fh.seek(spstart + 8)
                                    info['sample_count'] = struct.unpack('>I', fh.read(4))[0]
                                elif stype == b'stsd' and spstart + 16 <= send:
                                    for etype, estart, ehdr, eend in iter_boxes(fh, spstart + 8, send):
                                        info['codec'] = etype.decode('latin1', 'replace')
                                        epstart = estart + ehdr
                                        if etype in VISUAL_SAMPLE_ENTRIES and epstart + 78 <= eend:
                                            fh.seek(epstart + 24)
                                            cw, ch = struct.unpack('>HH', fh.read(4))
                                            info['coded_w'] = cw
                                            info['coded_h'] = ch
                                            for ctype, cstart, chdr, cend in iter_boxes(fh, epstart + 78, eend):
                                                if ctype == b'pasp' and cstart + chdr + 8 <= cend:
                                                    fh.seek(cstart + chdr)
                                                    hs, vs = struct.unpack('>II', fh.read(8))
                                                    info['pasp'] = (hs, vs)
                                        break
    return info


def inspect_mp4(path: Path) -> list:
    file_size = path.stat().st_size
    tracks = []
    with path.open('rb') as fh:
        for btype, bstart, bhdr, bend in iter_boxes(fh, 0, file_size):
            if btype == b'moov':
                for mtype, mstart, mhdr, mend in iter_boxes(fh, bstart + bhdr, bend):
                    if mtype == b'trak':
                        tracks.append(parse_track(fh, mstart, mhdr, mend))
    return tracks


def print_report(path: Path, tracks: list):
    print(f'File: {path} ({path.stat().st_size / (1024 * 1024):.2f} MiB)')
    if not tracks:
        print('  [!] No ISO-BMFF moov/trak atoms found.')
        return
    has_rtmd = False
    has_other_meta = False
    video_trk = None
    for idx, t in enumerate(tracks, 1):
        h = t['handler'] or '?'
        c = t['codec'] or '?'
        extra = []
        if t['coded_w'] and t['coded_h']:
            extra.append(f"coded={t['coded_w']}x{t['coded_h']}")
        if t['tkhd_w'] is not None and t['tkhd_h'] is not None and (t['tkhd_w'] > 0 or t['tkhd_h'] > 0):
            dar = (t['tkhd_w'] / t['tkhd_h']) if t['tkhd_h'] else 0
            extra.append(f"tkhd={t['tkhd_w']:.2f}x{t['tkhd_h']:.2f} (DAR {dar:.4f})")
        if t['pasp']:
            extra.append(f"pasp={t['pasp'][0]}:{t['pasp'][1]}")
        if t['sample_count'] is not None:
            extra.append(f"samples={t['sample_count']}")
        print(f"  Track #{idx}: handler='{h}' codec='{c}' " + ', '.join(extra))
        if h == 'vide' and video_trk is None:
            video_trk = t
        if c == 'rtmd' or h == 'rtmd':
            has_rtmd = True
        elif h == 'meta':
            has_other_meta = True

    if video_trk:
        tw, th = video_trk['tkhd_w'] or 0, video_trk['tkhd_h'] or 0
        dar = (tw / th) if th else 0
        is_3x2 = abs(dar - 1.5) < 0.02
        rtmd_status = (
            'PRESENT (Catalyst / Gyroflow compatible)'
            if has_rtmd
            else ('Not detected (other meta track present)' if has_other_meta else 'Not detected')
        )
        print('\nSummary:')
        print(f"  - Video Display Aspect : {'3:2 Open Gate (READY)' if is_3x2 else f'{dar:.3f} (not yet 3:2 in tkhd)'}")
        print(f"  - Sony RTMD Gyro/Lens  : {rtmd_status}")


def tag_3x2(src: Path, dst: Path, inplace: bool, mode: str):
    tracks = inspect_mp4(src)
    video_tracks = [t for t in tracks if t['handler'] == 'vide' and t['tkhd_w_offset'] is not None]
    if not video_tracks:
        sys.exit('Error: no video track with tkhd found.')
    vt = video_tracks[0]
    cw = vt['coded_w'] or int(round(vt['tkhd_w'] or 3840))
    ch = vt['coded_h'] or int(round(vt['tkhd_h'] or 2160))

    if mode == 'vertical-expand':
        # Keep full 3840 horizontal width, expand display height to 2560 (3840 * 2 / 3)
        new_w = float(cw)
        new_h = round((cw * 2.0 / 3.0) * 65536) / 65536.0
    else:
        # Horizontal squeeze (3240 x 2160)
        new_w = round((ch * 1.5) * 65536) / 65536.0
        new_h = float(ch)

    target = src if inplace else dst
    if not inplace:
        if target.exists() and target.resolve() == src.resolve():
            sys.exit('Error: output path matches input path; pass --inplace to modify in place.')
        print(f'Copying {src.name} -> {target.name} ...')
        shutil.copy2(src, target)

    w_fp = int(round(new_w * 65536.0))
    h_fp = int(round(new_h * 65536.0))
    with target.open('r+b') as fh:
        fh.seek(vt['tkhd_w_offset'])
        fh.write(struct.pack('>II', w_fp, h_fp))

    print(f'Updated {target.name}: tkhd {vt["tkhd_w"]:.2f}x{vt["tkhd_h"]:.2f} -> {new_w:.2f}x{new_h:.2f} (3:2 Open Gate, zero re-encode).')


def print_ffmpeg_commands(src: Path):
    stem = src.stem
    print('# 1) Lossless / Visually-Lossless Apple ProRes 422 HQ (3840x2560 square pixels, 10-bit 4:2:2):')
    print(
        f'ffmpeg -i "{src}" -vf "scale=3840:2560:flags=lanczos,setsar=1:1" '
        f'-c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le -c:a copy "{stem}_3840x2560_prores.mov"\n'
    )
    print('# 2) High-Bitrate 10-bit HEVC (macOS Hardware VideoToolbox, 3840x2560 square pixels):')
    print(
        f'ffmpeg -i "{src}" -vf "scale=3840:2560:flags=lanczos,setsar=1:1" '
        f'-c:v hevc_videotoolbox -profile:v main10 -b:v 150M -tag:v hvc1 -c:a copy "{stem}_3840x2560_hevc.mp4"'
    )


def main():
    parser = argparse.ArgumentParser(
        description='Inspect or losslessly tag Sony A7S III Open Gate 3:2 MP4/MOV recordings.'
    )
    sub = parser.add_subparsers(dest='cmd', required=True)

    p_ins = sub.add_parser('inspect', help='Inspect coded frame, tkhd display ratio, pasp, and Sony RTMD gyro metadata')
    p_ins.add_argument('input', type=Path, help='Input MP4/MOV file')

    p_tag = sub.add_parser('tag-3x2', help='Set MP4 tkhd display dimensions to 3:2 (3840x2560) with zero re-encoding')
    p_tag.add_argument('input', type=Path, help='Input MP4/MOV file')
    p_tag.add_argument('-o', '--output', type=Path, help='Output MP4/MOV file (default: <name>_3x2.MP4)')
    p_tag.add_argument('--inplace', action='store_true', help='Patch tkhd in-place (instant, 0 extra disk space)')
    p_tag.add_argument(
        '--mode',
        choices=['vertical-expand', 'horizontal-squeeze'],
        default='vertical-expand',
        help='vertical-expand -> 3840x2560 (default); horizontal-squeeze -> 3240x2160',
    )

    p_ff = sub.add_parser('ffmpeg-cmd', help='Print 10-bit 3840x2560 square-pixel FFmpeg export commands')
    p_ff.add_argument('input', type=Path, help='Input MP4/MOV file')

    args = parser.parse_args()
    if not args.input.is_file() and args.cmd != 'ffmpeg-cmd':
        sys.exit(f'File not found: {args.input}')

    if args.cmd == 'inspect':
        print_report(args.input, inspect_mp4(args.input))
    elif args.cmd == 'tag-3x2':
        out = args.output or args.input.with_name(f'{args.input.stem}_3x2{args.input.suffix}')
        tag_3x2(args.input, out, args.inplace, args.mode)
    elif args.cmd == 'ffmpeg-cmd':
        print_ffmpeg_commands(args.input)


if __name__ == '__main__':
    main()
