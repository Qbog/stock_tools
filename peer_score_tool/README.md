# peer_score_tool

把“抓数 → 评分 → 生成带颜色的 xlsx”拆成多个单一职责的命令行程序（偏 Linux / bash pipeline 风格）。

## 依赖
- Python 3
- `pandas`, `akshare`, `XlsxWriter`

## 配置
编辑 `config/cybersec_example.json`：
- 可增减类别/公司/指标科目
- 可配置排序方向（高为好/低为好）、并列规则、缺失值处理、评分档位

## 用法（一步一步）

### 1) 抓取原始指标（整洁表，tidy CSV）
```bash
python3 bin/ps_fetch_em.py --config config/cybersec_example.json \
  --report-date 2025-09-30 \
  --out /tmp/raw.csv
```

### 2) 计算排名与得分（tidy CSV）
```bash
python3 bin/ps_score.py --config config/cybersec_example.json \
  --in /tmp/raw.csv \
  --out /tmp/score.csv
```

### 3) 生成 xlsx（含着色）
```bash
python3 bin/ps_xlsx.py --config config/cybersec_example.json \
  --raw /tmp/raw.csv --score /tmp/score.csv \
  --out /tmp/cybersec_peers_2025-09-30.xlsx
```

## 输出说明
- raw.csv: 每一行 = (类别, 公司, 指标科目) 的原值 + 实际使用的报告期
- score.csv: 在 raw.csv 基础上增加 rank/score
- xlsx: 每个类别 2 张表：`*-得分` 和 `*-原值`，并带总分排序

## 重要
- 本工具只负责“把你定义的规则自动化执行”，不做投资建议。
- 数据口径以配置中的 `source_col` 为准；如果你要换数据源（Tushare、Wind 等），只需要替换 fetch 步骤即可。
