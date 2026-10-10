"""Summarizes a battle test log (R2_BATTLE_TEST=ai,log; see docs/rush2049_research/battle.md section 9).

Usage: python tools/battle_ai_log.py RUN_LOG
Prints, per computer car, its goals' shares, mean speed and the share of seconds spent nearly stopped, and the
damage and kills each car dealt (from the battle's "car N takes X from car M (health H)" lines).
"""
import re
import sys
from collections import Counter, defaultdict


def main(path):
    goals = defaultdict(Counter)
    speeds = defaultdict(list)
    dealt = Counter()
    kills = Counter()
    by_amount = Counter()
    ai = re.compile(r'\[BattleAI\] car (\d+) (\w+) .* v (-?\d+) ')
    hit = re.compile(r'car (\d+) takes (\d+) from car (-?\d+) \(health (\d+)\)')
    for line in open(path, errors='replace'):
        m = ai.search(line)
        if m:
            car = int(m.group(1))
            goals[car][m.group(2)] += 1
            speeds[car].append(abs(int(m.group(3))))
            continue
        m = hit.search(line)
        if m:
            amount, attacker, health = int(m.group(2)), int(m.group(3)), int(m.group(4))
            dealt[attacker] += amount
            by_amount[amount] += 1
            if health - amount <= 0:
                kills[attacker] += 1
    for car in sorted(goals):
        n = sum(goals[car].values())
        share = ', '.join('%s %d%%' % (g, 100 * c // n) for g, c in goals[car].most_common())
        v = speeds[car]
        print('car %d: %s; mean speed %.0f ft/s, stopped %d%%' % (car, share, sum(v) / len(v), 100 * sum(1 for s in v if s < 3) // len(v)))
    for car in sorted(set(dealt) | set(kills)):
        print('car %d dealt %d damage, %d kills' % (car, dealt[car], kills[car]))
    print('hits by amount:', dict(by_amount.most_common()))


if __name__ == '__main__':
    main(sys.argv[1])
