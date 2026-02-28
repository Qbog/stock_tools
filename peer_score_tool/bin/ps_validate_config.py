#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Validate config JSON.

Single responsibility: fail fast before long fetch.
"""

from __future__ import annotations

import argparse

import os, sys
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from lib.config import load_config


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    args = ap.parse_args()

    cfg = load_config(args.config)
    print("OK")
    print(f"categories={len(cfg.categories)} metrics={len(cfg.metrics)}")


if __name__ == "__main__":
    main()
