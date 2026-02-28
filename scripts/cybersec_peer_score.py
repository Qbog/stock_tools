#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Cybersecurity peers scoring (A-share) using Eastmoney financial indicators via Akshare.

Output: xlsx with score tables (1-5) color-coded.

Scoring rule (per metric, within each category):
- Rank companies by metric (direction depends on metric).
- Score = max(6 - rank, 1)
  (so only 5 score levels: 5,4,3,2,1; ranks >=5 all get 1)

Data source:
- akshare.stock_financial_analysis_indicator_em(symbol="300454.SZ", indicator="按报告期")

Notes:
- We default to latest full-year report (YYYY-12-31) available for each company.
  You can override with --report-date YYYY-MM-DD.

This script is for research/education; not investment advice.
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
import os
import time
from dataclasses import dataclass
from typing import Dict, List, Tuple

import pandas as pd

try:
    import akshare as ak
except Exception as e:
    raise SystemExit(f"akshare import failed: {e}")


SOFTWARE = [
    ("688651", "盛邦安全"),
    ("688561", "奇安信"),
    ("688244", "永信至诚"),
    ("688225", "亚信安全"),
    ("688201", "信安世纪"),
    ("688171", "纬德信息"),
    ("688168", "安博通"),
    ("688030", "山石网科"),
    ("688023", "安恒信息"),
    ("603232", "格尔软件"),
    ("601360", "三六零"),
    ("300768", "迪普科技"),
    ("300579", "数字认证"),
    ("300454", "深信服"),
    ("300369", "绿盟科技"),
    ("300352", "北信源"),
    ("300311", "任子行"),
    ("300229", "拓尔思"),
    ("300188", "国投智能"),
    ("003029", "吉大正元"),
    ("002439", "启明星辰"),
    ("002212", "天融信"),
]

EQUIPMENT = [
    ("300287", "飞利信"),
    ("301165", "锐捷网络"),
    ("688489", "三未信安"),
    ("603496", "恒为信息"),
    ("300659", "中孚信息"),
    ("300386", "飞天诚信"),
    ("300324", "旋极信息"),
    ("002912", "中新赛克"),
    ("002268", "电科网安"),
]


def to_em_symbol(code6: str) -> str:
    """Convert 6-digit A-share code to Eastmoney symbol format used by Akshare: e.g. 300454.SZ"""
    if code6.startswith(("60", "68")):
        return f"{code6}.SH"
    return f"{code6}.SZ"


@dataclass
class Metric:
    group: str
    name: str
    col: str
    higher_better: bool


METRICS: List[Metric] = [
    # 成长能力
    Metric("成长能力", "净利润", "PARENTNETPROFIT", True),
    Metric("成长能力", "净利润同比增长率", "PARENTNETPROFITTZ", True),
    Metric("成长能力", "扣非净利润", "KCFJCXSYJLR", True),
    Metric("成长能力", "扣非净利润同比增长率", "KCFJCXSYJLRTZ", True),
    Metric("成长能力", "营业总收入", "TOTALOPERATEREVE", True),
    Metric("成长能力", "营业总收入同比增长率", "TOTALOPERATEREVETZ", True),

    # 每股指标
    Metric("每股指标", "基本每股收益", "EPSJB", True),
    Metric("每股指标", "每股净资产", "BPS", True),
    Metric("每股指标", "每股资本公积", "MGZBGJ", True),
    Metric("每股指标", "每股未分配利润", "MGWFPLR", True),
    Metric("每股指标", "每股经营现金流", "MGJYXJJE", True),

    # 盈利能力
    Metric("盈利能力", "销售毛利率", "XSMLL", True),
    Metric("盈利能力", "销售净利率", "XSJLL", True),
    Metric("盈利能力", "净资产收益率", "ROEJQ", True),

    # 营运能力
    Metric("营运能力", "营业周期", "OPERATE_CYCLE", False),
    Metric("营运能力", "存货周转率", "CHZZL", True),
    Metric("营运能力", "存货周转天数", "CHZZTS", False),
    Metric("营运能力", "应收账款周转天数", "YSZKZZTS", False),

    # 偿债能力
    Metric("偿债能力", "流动比率", "LD", True),
    Metric("偿债能力", "速动比率", "SD", True),
    Metric("偿债能力", "保守速动比率", "CASH_RATIO", True),
    Metric("偿债能力", "产权比率", "CQBL", False),
    Metric("偿债能力", "资产负载率", "ZCFZL", False),
]


def pick_row_for_report(df: pd.DataFrame, report_date: str | None) -> pd.Series:
    """Pick the row corresponding to report_date (YYYY-MM-DD). If report_date is None,
    pick latest full-year (YYYY-12-31) if present; else latest available."""
    if df is None or df.empty:
        raise ValueError("empty df")

    df2 = df.copy()
    # ensure REPORT_DATE is datetime
    if not pd.api.types.is_datetime64_any_dtype(df2["REPORT_DATE"]):
        df2["REPORT_DATE"] = pd.to_datetime(df2["REPORT_DATE"], errors="coerce")

    if report_date:
        target = pd.to_datetime(report_date)
        hit = df2[df2["REPORT_DATE"] == target]
        if not hit.empty:
            return hit.sort_values("NOTICE_DATE", ascending=False).iloc[0]
        # fallback to latest <= target
        hit = df2[df2["REPORT_DATE"] <= target]
        if not hit.empty:
            return hit.sort_values("REPORT_DATE", ascending=False).iloc[0]
        return df2.sort_values("REPORT_DATE", ascending=False).iloc[0]

    # default: latest full-year
    full_year = df2[df2["REPORT_DATE"].dt.month.eq(12) & df2["REPORT_DATE"].dt.day.eq(31)]
    if not full_year.empty:
        return full_year.sort_values("REPORT_DATE", ascending=False).iloc[0]
    return df2.sort_values("REPORT_DATE", ascending=False).iloc[0]


def safe_float(x):
    try:
        if x is None:
            return math.nan
        if isinstance(x, str) and x.strip() == "":
            return math.nan
        v = float(x)
        return v
    except Exception:
        return math.nan


def fetch_company_metrics(code6: str, report_date: str | None, pause_s: float = 0.25, retries: int = 2) -> Tuple[Dict[str, float], str]:
    """Fetch a single company's metric values. Returns (metric_values, used_report_date_str)."""
    symbol = to_em_symbol(code6)
    last_err = None
    for attempt in range(retries + 1):
        try:
            df = ak.stock_financial_analysis_indicator_em(symbol=symbol, indicator="按报告期")
            row = pick_row_for_report(df, report_date)
            used_date = pd.to_datetime(row["REPORT_DATE"]).strftime("%Y-%m-%d")
            out = {}
            for m in METRICS:
                out[m.name] = safe_float(row.get(m.col))
            time.sleep(pause_s)
            return out, used_date
        except Exception as e:
            last_err = e
            time.sleep(pause_s * (attempt + 1))
    raise RuntimeError(f"fetch failed for {code6} ({symbol}): {last_err}")


def score_by_metric(values: Dict[str, float], higher_better: bool) -> Dict[str, int]:
    """Given company->value for a metric, return company->score 1..5."""
    items = [(k, v) for k, v in values.items() if v is not None and not (isinstance(v, float) and math.isnan(v))]
    # If everything missing, return all 1
    if not items:
        return {k: 1 for k in values.keys()}

    # Sort
    items_sorted = sorted(items, key=lambda kv: kv[1], reverse=higher_better)

    # Rank (1-based). Missing values get worst rank.
    scores: Dict[str, int] = {}
    for idx, (k, _v) in enumerate(items_sorted, start=1):
        scores[k] = max(6 - idx, 1)

    for k, v in values.items():
        if k not in scores:
            scores[k] = 1
    return scores


def build_tables(companies: List[Tuple[str, str]], report_date: str | None) -> Tuple[pd.DataFrame, pd.DataFrame, pd.Series]:
    """Return (raw_df, score_df, used_dates). Index=metrics, columns=company display."""
    # Fetch raw
    raw: Dict[str, Dict[str, float]] = {}
    used_dates: Dict[str, str] = {}

    for code6, cname in companies:
        disp = f"{cname}({code6})"
        vals, used = fetch_company_metrics(code6, report_date)
        raw[disp] = vals
        used_dates[disp] = used

    raw_df = pd.DataFrame(raw).reindex([m.name for m in METRICS])

    # Score table
    score_df = pd.DataFrame(index=[m.name for m in METRICS], columns=raw_df.columns, dtype="int")

    for m in METRICS:
        metric_vals = {col: safe_float(raw_df.loc[m.name, col]) for col in raw_df.columns}
        s = score_by_metric(metric_vals, m.higher_better)
        for col, sc in s.items():
            score_df.loc[m.name, col] = sc

    # Totals
    totals = score_df.sum(axis=0)

    # Sort columns by total desc
    order = totals.sort_values(ascending=False).index.tolist()
    score_df = score_df[order]
    raw_df = raw_df[order]
    totals = totals[order]

    used_dates_s = pd.Series(used_dates)[order]

    return raw_df, score_df, used_dates_s


def write_xlsx(path: str, tables: Dict[str, Tuple[pd.DataFrame, pd.DataFrame, pd.Series]], report_date: str | None):
    import xlsxwriter

    os.makedirs(os.path.dirname(path), exist_ok=True)

    with pd.ExcelWriter(path, engine="xlsxwriter") as writer:
        wb = writer.book

        # formats
        fmt_title = wb.add_format({"bold": True, "font_size": 14})
        fmt_note = wb.add_format({"font_size": 10, "font_color": "#666666"})
        fmt_hdr = wb.add_format({"bold": True, "bg_color": "#F2F2F2", "border": 1})
        fmt_metric = wb.add_format({"bold": True, "border": 1})
        fmt_cell = wb.add_format({"border": 1})
        fmt_cell_num = wb.add_format({"border": 1, "num_format": "0.00"})

        color_map = {
            5: "#63BE7B",  # green
            4: "#A9D18E",  # light green
            3: "#FFEB84",  # yellow
            2: "#F4B183",  # orange
            1: "#F8696B",  # red
        }

        for cat, (raw_df, score_df, used_dates) in tables.items():
            # Score sheet
            sheet_scores = f"{cat}-得分"
            score_df.to_excel(writer, sheet_name=sheet_scores, startrow=4, startcol=1)
            ws = writer.sheets[sheet_scores]

            ws.write(0, 1, f"网络安全公司横向对比（{cat}）- 评分表", fmt_title)
            ws.write(1, 1, f"报告期: {report_date or '最新年报(自动选择)'}；评分规则: rank->score=max(6-rank,1)；仅用于研究，不构成投资建议。", fmt_note)
            ws.write(2, 1, "公司数据实际使用的报告期（如个别公司缺失目标期则自动回退）：", fmt_note)
            ws.write(3, 1, "； ".join([f"{k}:{v}" for k, v in used_dates.items()])[:32000], fmt_note)

            # Add totals row
            total_row = 4 + len(METRICS) + 1  # header row included by to_excel
            # Pandas writes header at row 4, metrics start at row 5
            ws.write(total_row, 1, "总分", fmt_metric)
            for j, col in enumerate(score_df.columns, start=2):
                ws.write_number(total_row, j, int(score_df[col].sum()), fmt_cell)

            # Styling: borders & header
            # Freeze panes below header row and metrics column
            ws.freeze_panes(5, 2)

            # Set column widths
            ws.set_column(1, 1, 22)  # metric names
            ws.set_column(2, 2 + len(score_df.columns), 16)

            # Apply conditional formats to score area (metrics rows only)
            first_row = 5
            last_row = first_row + len(METRICS) - 1
            first_col = 2
            last_col = first_col + len(score_df.columns) - 1

            rng = xlsxwriter.utility.xl_range(first_row, first_col, last_row, last_col)
            for sc, color in color_map.items():
                ws.conditional_format(rng, {
                    "type": "cell",
                    "criteria": "==",
                    "value": sc,
                    "format": wb.add_format({"bg_color": color, "border": 1})
                })

            # Total row conditional formatting too
            total_rng = xlsxwriter.utility.xl_range(total_row, first_col, total_row, last_col)
            ws.conditional_format(total_rng, {
                "type": "2_color_scale",
                "min_color": "#F8696B",
                "max_color": "#63BE7B",
            })

            # Raw sheet
            sheet_raw = f"{cat}-原值"
            raw_df.to_excel(writer, sheet_name=sheet_raw, startrow=3, startcol=1)
            ws2 = writer.sheets[sheet_raw]
            ws2.write(0, 1, f"网络安全公司横向对比（{cat}）- 原始指标", fmt_title)
            ws2.write(1, 1, f"报告期: {report_date or '最新年报(自动选择)'}；来源: 东方财富/akshare。", fmt_note)
            ws2.freeze_panes(4, 2)
            ws2.set_column(1, 1, 22)
            ws2.set_column(2, 2 + len(raw_df.columns), 16)

        # Add a README sheet
        ws = wb.add_worksheet("说明")
        ws.write(0, 0, "说明", fmt_title)
        ws.write(2, 0, "1) 本表为横向对比打分：每个科目按同类别公司排序，rank->score=max(6-rank,1)，因此只有 5/4/3/2/1 五档颜色。", fmt_note)
        ws.write(3, 0, "2) 部分指标可能为负值或缺失；缺失值统一按最低分(1)处理。", fmt_note)
        ws.write(4, 0, "3) 仅用于研究，不构成任何投资建议；最终决策需结合行业景气、产品竞争力、治理、现金流与估值。", fmt_note)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report-date", default=None, help="Report date YYYY-MM-DD (e.g. 2024-12-31). Default: latest full-year available")
    ap.add_argument("--out", default=None, help="Output xlsx path")
    args = ap.parse_args()

    report_date = args.report_date
    if report_date:
        # validate
        dt.datetime.strptime(report_date, "%Y-%m-%d")

    out = args.out
    if not out:
        today = dt.datetime.now().strftime("%Y%m%d")
        out = f"/root/.openclaw/workspace/output/cybersec_peers_scoring_{today}.xlsx"

    tables = {}
    raw_s, score_s, dates_s = build_tables(SOFTWARE, report_date)
    tables["软件开发"] = (raw_s, score_s, dates_s)

    raw_e, score_e, dates_e = build_tables(EQUIPMENT, report_date)
    tables["安全设备"] = (raw_e, score_e, dates_e)

    write_xlsx(out, tables, report_date)
    print(out)


if __name__ == "__main__":
    main()
