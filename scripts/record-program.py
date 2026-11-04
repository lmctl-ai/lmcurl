#!/usr/bin/env python3
"""record-program.py HOST OUTPUT --lmcode FILE: keep a copy of the private program file, then run the host."""
import shutil
import subprocess
import sys

host, output, *args = sys.argv[1:]
shutil.copyfile(args[1], output)
sys.exit(subprocess.call([host, *args]))
