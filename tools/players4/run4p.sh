#!/bin/sh
# Runs a 3/4 player test (test aid): sets the saved track select to a Rush 2 track, launches the game through
# tools/rush1/shots.py with test players filling ports 3 and 4, and restores the track select afterwards.
# usage: tools/players4/run4p.sh OUTDIR "<input script>" t1 t2 ...   (TP=0 for no test players;
#        RUSH2_TEST_FINISH=<seconds> finishes the human cars that long into the race, see src/players4.cpp)
cd "$(dirname "$0")/../.."
cp build/saves/rush2.n64.us.json tmp/save_backup.json
py - <<'P'
import json
p='build/saves/rush2.n64.us.json'; d=json.load(open(p)); d['track_select']['selected']=-1; json.dump(d,open(p,'w'),indent=4)
P
out=$1; shift; script=$1; shift
RUSH2_TEST_PLAYERS=${TP:-4} py tools/rush1/shots.py "$out" "$script" "$@"
py - <<'P'
import json
b=json.load(open('tmp/save_backup.json'))['track_select']['selected']
p='build/saves/rush2.n64.us.json'; d=json.load(open(p)); d['track_select']['selected']=b; json.dump(d,open(p,'w'),indent=4)
P
