#!/usr/bin/env python3
"""Add the perseus entry to the HackiOS loader's device table.

usage: add-platform.py <loader.c> <perseus.platform.c>

The loader picks its `struct platform` by the framebuffer base the firmware reports; on this
phone that is 0x9d400000 (SimpleFbDxe). The entry itself is device/perseus.platform.c.
"""
import sys

loader_path, platform_path = sys.argv[1], sys.argv[2]
src = open(loader_path).read()

if 'platform_perseus' in src:
    print('platform_perseus already in', loader_path)
    sys.exit(0)

entry = open(platform_path).read().rstrip() + '\n\n'
anchor = 'static const struct platform *const platforms[] = {'
if anchor not in src:
    raise SystemExit('anchor not found: the device table in loader.c moved')
src = src.replace(anchor, entry + anchor, 1)

old_table = '\t&platform_redwood,\n};'
if old_table not in src:
    raise SystemExit('anchor not found: platforms[] no longer starts with platform_redwood')
src = src.replace(old_table, '\t&platform_redwood,\n\t&platform_perseus,\n};', 1)

open(loader_path, 'w').write(src)
print('added platform_perseus to', loader_path)
