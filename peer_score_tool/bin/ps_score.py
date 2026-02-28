#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Compute ranks and scores from tidy raw CSV.

Single responsibility: scoring.

Input: CSV from ps_fetch_em.py
Output: same CSV + rank, score
"""

from __future__ import annotations

import argparse
import sys

import pandas as pd

import os, sys
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from lib.config import load_config
from lib.scoring import tidy_score


def read_csv(path: str) -> pd.DataFrame:
    if path == "-":
        return pd.read_csv(sys.stdin)
    return pd.read_csv(path)


def write_csv(df: pd.DataFrame, path: str):
    if path == "-":
        df.to_csv(sys.stdout, index=False)
    else:
        df.to_csv(path, index=False, encoding="utf-8-sig")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    ap.add_argument("--in", dest="inp", required=True, help="Input raw CSV path or -")
    ap.add_argument("--out", required=True, help="Output score CSV path or -")
    args = ap.parse_args()

    cfg = load_config(args.config)
    df = read_csv(args.inp)

    # normalize types
    df = df.copy()
    df["higher_better"] = df["higher_better"].astype(int).astype(bool)

    df_sc = tidy_score(df.rename(columns={
        "metric": "metric",
        "company": "company",
        "category": "category",
        "value": "value",
    }), cfg.scoring)

    write_csv(df_sc, args.out)


if __name__ == "__main__":
    main()
