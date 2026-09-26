#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Whykiki Audio
"""Command-line entry point; source_release is also importable by the packager."""
import sys
sys.dont_write_bytecode = True

from source_release import main

if __name__ == "__main__":
    main()
