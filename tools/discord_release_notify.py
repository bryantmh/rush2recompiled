"""Post a release changelog to a Discord channel through an incoming webhook.

tools/release.py calls announce() right after the GitHub release is created, with the same change list the release page
shows. It also runs standalone against a notes file, which is how to repost a release or check the formatting:

    py tools/discord_release_notify.py --notes notes.md --tag v1.0.17 --repo bryantmh/rush2recompiled
    py tools/discord_release_notify.py --notes notes.md --tag v1.0.17 --dry-run

The webhook URL comes from $DISCORD_WEBHOOK_URL, or else the gitignored file .discord_webhook in the repo root. It is
never taken on the command line (argv is visible to every process on the machine) and never committed. With neither
set, the announcement is skipped and the release goes ahead as normal.

Uses urllib rather than requests so it needs nothing beyond the standard library.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEBHOOK_FILE = ROOT / ".discord_webhook"

# Discord rejects the whole payload (HTTP 400) when an embed description exceeds 4096 characters, so a big release
# would lose its announcement entirely. Stay under the cap with room for the truncation marker.
BODY_BUDGET = 3800
TRUNCATION_MARKER = "... (truncated, see the release page for the full list)"

# GitHub's release green, so the embed stripe matches the source of the notes.
EMBED_COLOR = 0x2EA043


def webhook_url():
    url = os.environ.get("DISCORD_WEBHOOK_URL", "").strip()
    if not url and WEBHOOK_FILE.is_file():
        url = WEBHOOK_FILE.read_text(encoding="utf-8").strip()
    return url


def truncate(body, budget=BODY_BUDGET):
    """Trims body to budget characters on a line boundary, so no commit subject is cut in half."""
    if len(body) <= budget:
        return body
    head = body[:budget]
    if "\n" in head:
        head = head.rsplit("\n", 1)[0]
    return head.rstrip() + "\n" + TRUNCATION_MARKER


def build_payload(notes, tag, repo=None):
    """Discord webhook JSON for one release. The notes are markdown ("- subject" lines), which Discord renders as-is."""
    embed = {"title": f"Rush 2 Recompiled {tag}", "description": truncate(notes.strip()), "color": EMBED_COLOR}
    if repo:
        embed["url"] = f"https://github.com/{repo}/releases/tag/{tag}"
    return {"embeds": [embed]}


def post(url, payload, timeout=20):
    request = urllib.request.Request(url, data=json.dumps(payload).encode("utf-8"), method="POST", headers={
        "Content-Type": "application/json",
        # Discord 403s urllib's default agent on some edge nodes.
        "User-Agent": "rush2recompiled-release-notifier/1.0",
    })
    with urllib.request.urlopen(request, timeout=timeout) as response:
        # A webhook POST answers 204 No Content on success.
        if response.status not in (200, 204):
            raise RuntimeError(f"Discord returned HTTP {response.status}")


def announce(notes, tag, repo=None):
    """Posts the release to Discord. Returns True on success; a failure is reported but never raised, because the
    release already exists by the time this runs and a Discord outage must not fail the push."""
    url = webhook_url()
    if not url:
        print("No Discord webhook (DISCORD_WEBHOOK_URL or .discord_webhook); skipping the announcement.")
        return False
    try:
        post(url, build_payload(notes, tag, repo))
    except urllib.error.HTTPError as exc:
        # Discord explains a 400 in the body; without it the failure is unactionable.
        detail = exc.read().decode("utf-8", "replace")[:500]
        print(f"Discord rejected the announcement: HTTP {exc.code} {detail}", file=sys.stderr)
        return False
    except (urllib.error.URLError, RuntimeError, OSError) as exc:
        print(f"Could not reach Discord: {exc}", file=sys.stderr)
        return False
    print(f"Announced {tag} on Discord.")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--notes", required=True, help="markdown file holding the change list to post")
    ap.add_argument("--tag", required=True, help="release tag, e.g. v1.0.17")
    ap.add_argument("--repo", help="owner/name, used to link the embed to the release page")
    ap.add_argument("--dry-run", action="store_true", help="print the payload instead of posting it")
    args = ap.parse_args()

    notes = Path(args.notes).read_text(encoding="utf-8")
    if args.dry_run:
        print(json.dumps(build_payload(notes, args.tag, args.repo), indent=2))
        return 0
    return 0 if announce(notes, args.tag, args.repo) else 1


if __name__ == "__main__":
    raise SystemExit(main())
