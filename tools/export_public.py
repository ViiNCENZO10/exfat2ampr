#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
"""Export the working tree to the public repository, without the private history.

usage: export_public.py PUBLIC_CLONE

PUBLIC_CLONE is a clone of the public repository (or an empty folder for the first export). The tracked files
of HEAD replace its content and become one new commit, signed "V-10" with an address that belongs to no GitHub
account (.invalid can never exist): the commits show the name only, linked to nobody's profile. VERSION_OFFSET is written so that the public version number (1.<offset + commits>) equals
this repository's: the app's version keeps growing across the switch, and its updates compare correctly."""
import os
import shutil
import subprocess
import sys

AUTHOR = "V-10 <v-10@exfat2ampr.invalid>"
here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dst = os.path.abspath(sys.argv[1])


def git(*args, cwd, out=False):
    r = subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True)
    return r.stdout.strip() if out else None


count = int(git("rev-list", "--count", "HEAD", cwd=here, out=True))
os.makedirs(dst, exist_ok=True)
if not os.path.isdir(os.path.join(dst, ".git")):
    git("init", "-q", "-b", "main", cwd=dst)
public_before = 0
try:
    public_before = int(git("rev-list", "--count", "HEAD", cwd=dst, out=True))
except subprocess.CalledProcessError:
    pass   # empty repository: the first commit

# the public tree: exactly the tracked files of HEAD
for name in os.listdir(dst):
    if name != ".git":
        p = os.path.join(dst, name)
        shutil.rmtree(p) if os.path.isdir(p) and not os.path.islink(p) else os.remove(p)
archive = subprocess.run(["git", "archive", "--format=tar", "HEAD"], cwd=here, check=True, capture_output=True).stdout
subprocess.run(["tar", "-x", "-C", dst], input=archive, check=True)
offset = count - (public_before + 1)
with open(os.path.join(dst, "VERSION_OFFSET"), "w", newline="\n") as f:
    f.write(f"{offset}\n")

git("add", "-A", cwd=dst)
env_author = AUTHOR.split(" <")
msg = f"exFAT2AMPR 1.{count}"
subprocess.run(["git", "-c", f"user.name={env_author[0]}", "-c", f"user.email={env_author[1][:-1]}", "commit", "-q",
                "--author", AUTHOR, "-m", msg], cwd=dst, check=True)
print(f"exported HEAD ({count} private commits) as public commit {public_before + 1}, VERSION_OFFSET={offset}, version 1.{count}")
