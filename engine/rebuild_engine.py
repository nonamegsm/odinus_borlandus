#!/usr/bin/env python3
"""Build the native classic BCC engine as part of the standalone application."""
from pathlib import Path
import subprocess
import sys
root = Path(__file__).resolve().parent.parent
sys.exit(subprocess.call([sys.executable, str(root / 'build.py')] + sys.argv[1:], cwd=root))
