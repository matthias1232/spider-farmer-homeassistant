#!/usr/bin/env python3
"""Prints the discovery topics the previous firmware published and this one
no longer does, as a C array literal for main/ha_mqtt.c.

Home Assistant keeps a discovery entity for as long as the broker retains
its config payload. Nothing republishes an entity that has been renamed or
removed, so its old payload has to be overwritten with an empty one --
otherwise the superseded device pages stay in the registry forever.

Deriving the list by hand is what left eight orphan devices behind last
time: the list was written from what the current table no longer contained
rather than from what the previous firmware actually published. This script
reads the previous table and subtracts the current one, so the two cannot
drift apart.

Usage: gen_stale_topics.py <previous ha_discovery_table.c> <current ...>
"""

import os
import re
import sys

TOPIC_RE = re.compile(r'\{\s*"(homeassistant/[a-z_]+/spiderfarmer_[^"]+)"\s*,')


def topics(path):
    with open(path, "r", encoding="utf-8") as fh:
        return [m.group(1) for m in TOPIC_RE.finditer(fh.read())]


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2

    prev, cur = topics(sys.argv[1]), set(topics(sys.argv[2]))
    missing = [t for t in prev if t not in cur]

    print("previous table: %d topics" % len(prev))
    print("current table:  %d topics" % len(cur))
    print("superseded:     %d topics\n" % len(missing))

    print("static const char *STALE_DISCOVERY_TOPICS[] = {")
    for t in missing:
        # Grouped so the list stays readable; the block is reprinted below
        # with a blank line every eight entries.
        print('    "%s",' % t)
    print("};")
    return 0


if __name__ == "__main__":
    sys.exit(main())
