#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build XLSX workbook from raw + score tidy CSV.

Single responsibility: presentation (pivot + conditional formatting).

Sheets per category:
- {category}-得分
- {category}-原值
"""

from __future__ import annotations

import argparse

import pandas as pd

import os, sys
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from lib.config import load_config
from lib.xlsx import write_workbook


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    ap.add_argument("--raw", required=True)
    ap.add_argument("--score", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--report-date-label", default=None, help="Label to show in xlsx title")
    args = ap.parse_args()

    cfg = load_config(args.config)
    df_raw = pd.read_csv(args.raw)
    df_sc = pd.read_csv(args.score)

    # pivot into matrices per category
    tables = {}

    for cat in [c.name for c in cfg.categories]:
        sub_r = df_raw[df_raw["category"] == cat]
        sub_s = df_sc[df_sc["category"] == cat]

        raw_mx = sub_r.pivot(index="metric", columns="company", values="value")
        score_mx = sub_s.pivot(index="metric", columns="company", values="score")

        # totals for sorting companies
        totals = score_mx.sum(axis=0).sort_values(ascending=False)
        order = totals.index.tolist()

        raw_mx = raw_mx[order]
        score_mx = score_mx[order]
        totals = totals[order]

        # keep metric order as in config
        metric_order = [m.name for m in cfg.metrics]
        raw_mx = raw_mx.reindex(metric_order)
        score_mx = score_mx.reindex(metric_order)

        tables[cat] = (raw_mx, score_mx, totals)

    report_label = args.report_date_label
    if not report_label:
        # infer from raw.csv (requested report date or used)
        req = str(df_raw["report_date_req"].dropna().iloc[0]) if "report_date_req" in df_raw.columns and not df_raw["report_date_req"].dropna().empty else ""
        report_label = req or "自动(公司各自最新/回退)"

    write_workbook(
        out_path=args.out,
        tables=tables,
        report_date_label=report_label,
        color_map=cfg.color_map,
        scoring_range=(cfg.scoring.min_score, cfg.scoring.max_score),
    )


if __name__ == "__main__":
    main()
