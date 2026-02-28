#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Fetch Eastmoney financial indicator row for each company and output tidy CSV.

Single responsibility: data extraction.

Output columns:
- category
- company (display: 名称(代码))
- code
- em_symbol
- metric_group
- metric
- source_col
- direction
- higher_better
- report_date_req
- report_date_used
- value
"""

from __future__ import annotations

import argparse
import csv
import sys
from typing import Dict, List

import pandas as pd

import os, sys
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from lib.config import load_config
from lib.em_source import fetch_em_indicator_row, to_em_symbol


def write_rows(path: str, rows: List[Dict]):
    if path == "-":
        out_f = sys.stdout
        close = False
    else:
        out_f = open(path, "w", encoding="utf-8", newline="")
        close = True

    try:
        w = csv.DictWriter(out_f, fieldnames=list(rows[0].keys())) if rows else None
        if w:
            w.writeheader()
            for r in rows:
                w.writerow(r)
    finally:
        if close:
            out_f.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    ap.add_argument("--report-date", default=None, help="YYYY-MM-DD; if missing, auto-pick latest full-year")
    ap.add_argument("--out", required=True, help="Output CSV path, or - for stdout")
    ap.add_argument("--pause", type=float, default=0.25)
    ap.add_argument("--retries", type=int, default=2)
    args = ap.parse_args()

    cfg = load_config(args.config)
    indicator = cfg.data_source.get("akshare_indicator", "按报告期")

    # metric name -> source col
    col_map = {m.name: m.source_col for m in cfg.metrics}
    meta = {m.name: m for m in cfg.metrics}

    rows: List[Dict] = []

    for cat in cfg.categories:
        for co in cat.companies:
            em_symbol = to_em_symbol(co.code)
            fr = fetch_em_indicator_row(
                code6=co.code,
                report_date=args.report_date,
                columns=col_map,
                indicator=indicator,
                pause_s=args.pause,
                retries=args.retries,
            )

            for m in cfg.metrics:
                rows.append({
                    "category": cat.name,
                    "company": co.display,
                    "code": co.code,
                    "em_symbol": em_symbol,
                    "metric_group": m.group,
                    "metric": m.name,
                    "source_col": m.source_col,
                    "direction": m.direction,
                    "higher_better": 1 if m.higher_better else 0,
                    "report_date_req": args.report_date or "",
                    "report_date_used": fr.used_report_date,
                    "value": fr.values.get(m.name),
                })

    if rows:
        write_rows(args.out, rows)


if __name__ == "__main__":
    main()
