"""Searches this project's Claude Code session transcripts for messages containing every given word (case-insensitive),
to find what was decided or fixed in an earlier session.

    python tools/transcript_search.py WORD... [--all] [--session PREFIX]

By default only the user's messages are searched; --all includes the assistant's replies. --session limits the search
to the transcripts whose file name starts with PREFIX (the session id). Prints the session, the time and the message.
"""

import argparse
import glob
import json
import os

TRANSCRIPTS = os.path.expanduser('~/.claude/projects/c--Users-Bryant-Documents-Code-rush2-recomp')


def texts(entry):
    content = entry.get('message', {}).get('content')
    if isinstance(content, str):
        return [content]
    return [x.get('text', '') for x in content or [] if isinstance(x, dict) and x.get('type') == 'text']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('words', nargs='+')
    ap.add_argument('--all', action='store_true', help="include the assistant's replies")
    ap.add_argument('--session', default='', help='only transcripts whose name starts with this')
    ap.add_argument('--max', type=int, default=1200, help='characters of each message to print')
    a = ap.parse_args()
    words = [w.lower() for w in a.words]
    kinds = ('user', 'assistant') if a.all else ('user',)
    for path in sorted(glob.glob(os.path.join(TRANSCRIPTS, a.session + '*.jsonl'))):
        with open(path, encoding='utf-8', errors='ignore') as f:
            for line in f:
                low = line.lower()
                if not all(w in low for w in words):   # cheap filter before parsing
                    continue
                try:
                    entry = json.loads(line)
                except ValueError:
                    continue
                if entry.get('type') not in kinds:
                    continue
                for t in texts(entry):
                    if all(w in t.lower() for w in words):
                        print(os.path.basename(path)[:8], entry.get('type'), entry.get('timestamp', '')[:16],
                              t[:a.max].replace('\n', ' '))
                        print('---')


if __name__ == '__main__':
    main()
