"""Matches music streams across Rush 2049 versions by pitch content (chroma), the way docs/rush2049_research/audio.md
paired the N64 songs with the Dreamcast disc's.

    song_match.py ARCADE_AUDIO_DIR DC_STR_DIR [--rate HZ] [--clip SECONDS]

ARCADE_AUDIO_DIR: the arcade disk's AUDIO folder (e.g. %LOCALAPPDATA%/ProjectR/Data/sf2049se/AUDIO). Its .str files are
'STRM', u32 data bytes, u32 0x36, u32 0x52, zeros to 512 bytes; the whole file is 5.1 PCM16 frames of 6 samples
(FL FR C LFE BL BR, LFE always 0) counted from byte 0, so the header is the first frames. Rate 22050 Hz [I]: its
spectrum falls off like the disc's 22050 Hz songs.
DC_STR_DIR: .STR files from the Dreamcast disc (tools/rush2049/dc_cdi.py get IMAGE NAME.STR OUT): a 0x800-byte header
(u32 1, rate, bits, block bytes, blocks, data bytes, channels, end block, loop block), then per block one 16 KB PCM16
block per channel.

For every arcade stream, a clip from its middle is slid over each disc song's chroma; prints the best disc song, its
mean frame correlation and the runner-up's. --rate is the arcade's sample rate (it only scales pitch and time).
"""
import argparse, glob, os, struct
import numpy as np

HOP = 0.1  # seconds per chroma frame


def chroma(mono, rate):
    n = 4096
    hop = int(rate * HOP)
    frames = (len(mono) - n) // hop
    if frames <= 0:
        return np.zeros((0, 12))
    win = np.hanning(n).astype(np.float32)
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    ok = (freqs > 55) & (freqs < 4000)
    pc = (np.round(12 * np.log2(freqs[ok] / 440.0)).astype(int) % 12)
    out = np.zeros((frames, 12), np.float32)
    for s in range(0, frames, 512):
        idx = np.arange(s, min(frames, s + 512))[:, None] * hop + np.arange(n)[None, :]
        mag = np.abs(np.fft.rfft(mono[idx] * win, axis=1))[:, ok]
        for k in range(12):
            out[s:s + len(idx), k] = mag[:, pc == k].sum(axis=1)
    out -= out.mean(axis=1, keepdims=True)
    out /= np.linalg.norm(out, axis=1, keepdims=True) + 1e-9
    return out


def read_arcade(path):
    b = np.fromfile(path, dtype='<i2', offset=512)
    b = b[:len(b) // 6 * 6].reshape(-1, 6).astype(np.float32)
    return b.mean(axis=1)


def read_dc(path):
    d = open(path, 'rb').read()
    rate, block, channels = struct.unpack_from('<I', d, 4)[0], struct.unpack_from('<I', d, 12)[0], 2
    pcm = np.frombuffer(d, dtype='<i2', offset=0x800)
    per = block // 2
    blocks = len(pcm) // (per * channels)
    pcm = pcm[:blocks * per * channels].reshape(blocks, channels, per).astype(np.float32)
    return pcm.mean(axis=1).reshape(-1), rate


def best_offset(clip, song):
    if len(song) < len(clip):
        return -1.0
    # mean over the clip of per-frame cosine similarity, for every offset
    w = len(clip)
    sims = np.zeros(len(song) - w + 1, np.float32)
    for k in range(12):
        sims += np.convolve(song[:, k], clip[::-1, k], mode='valid')
    return float(sims.max() / w)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('arcade')
    ap.add_argument('dc')
    ap.add_argument('--rate', type=int, default=22050)
    ap.add_argument('--clip', type=float, default=40.0)
    ap.add_argument('--wav', help='also write each arcade stream as a stereo WAV (5.1 folded down) into this folder')
    a = ap.parse_args()
    if a.wav:
        import wave
        os.makedirs(a.wav, exist_ok=True)
        for f in sorted(glob.glob(os.path.join(a.arcade, '*.str'))):
            x = np.fromfile(f, dtype='<i2')
            x = x[:len(x) // 6 * 6].reshape(-1, 6).astype(np.float32)  # FL FR C LFE BL BR, frames from byte 0
            st = np.stack([x[:, 0] + 0.7 * x[:, 2] + x[:, 4], x[:, 1] + 0.7 * x[:, 2] + x[:, 5]], axis=1) * 0.45
            out = os.path.join(a.wav, os.path.basename(f)[:-4] + '.wav')
            with wave.open(out, 'wb') as w:
                w.setnchannels(2); w.setsampwidth(2); w.setframerate(a.rate)
                w.writeframes(np.clip(st, -32768, 32767).astype('<i2').tobytes())
        return
    dc = {}
    for f in sorted(glob.glob(os.path.join(a.dc, '*.STR'))):
        mono, rate = read_dc(f)
        dc[os.path.basename(f)] = chroma(mono, rate)
    for f in sorted(glob.glob(os.path.join(a.arcade, '*.str'))):
        mono = read_arcade(f)
        c = chroma(mono, a.rate)
        secs = len(mono) / a.rate
        w = int(a.clip / HOP)
        if len(c) < w + 10:
            print('%-14s %6.1f s  (too short)' % (os.path.basename(f), secs))
            continue
        mid = len(c) // 2 - w // 2
        clip = c[mid:mid + w]
        scores = sorted(((best_offset(clip, s), n) for n, s in dc.items()), reverse=True)
        print('%-14s %6.1f s  best %-12s %.2f  next %-12s %.2f' % (os.path.basename(f), secs, scores[0][1],
              scores[0][0], scores[1][1], scores[1][0]))


if __name__ == '__main__':
    main()
