# SAT Competition Benchmarks

## 目錄結構

```
benchmarks/
├── sc2023/              # SC2023 main track (400 instances)
│   ├── instances.csv    # 索引：hash, cnf_path, expected, ...
│   └── *.cnf            # 僅保留解壓後的 .cnf
├── sc2024/              # SC2024 main track (400 instances)
├── sample100/           # 100 題子集 (50 SAT + 50 UNSAT)
│   └── instances.csv
├── track_main_2023.uri  # 下載 URI 列表
└── track_main_2024.uri
```

## 下載與驗證

```bash
python scripts/download_benchmarks.py              # 下載 SC2023+SC2024
python scripts/download_benchmarks.py --verify      # 驗證並重下損壞檔
python scripts/download_benchmarks.py --cleanup     # 結束後刪除 .cache、download.log
```

- 下載後自動驗證 CNF 完整性（最後一行須以 `0` 結尾）
- 解壓成功後刪除 .xz（預設，可用 `--no-delete-xz` 保留）
- 結束後自動產生 `track_main_{year}.uri`

## 檔案格式說明

### `.cnf`（DIMACS CNF）
- **用途**：SAT solver 直接讀取的純文字格式
- **內容**：變數、clause、布林公式（每行一個 clause，以 `0` 結尾）
- **範例**：`p cnf 10 5` + `1 -2 3 0` 表示變數 1、-2、3 的 OR
- **特點**：解壓後、可執行，占空間大

### `.xz`（XZ 壓縮檔）
- **用途**：`.cnf` 的壓縮儲存格式，用於下載與備份
- **內容**：與 `.cnf` 相同，但經 LZMA2 壓縮（約 50% 體積）
- **使用**：需先解壓成 `.cnf`，solver 無法直接讀取
- **特點**：省空間，需解壓後才能跑 benchmark

```
下載流程：.xz ──解壓──> .cnf ──執行──> solver
```

本專案目前只保留 `.cnf`，`.xz` 已刪除以節省約 50% 空間。

---

## 實驗腳本 (run_experiment.sh)

比對 **baseline**（Painless HordeSat `shr-strat=1`）與 **實驗組**（DSRG + GPU `shr-strat=4 -shr-gpu`）。

**重要：** 兩組必須**循序執行**，不可同時跑，避免 GPU/CPU 互相搶佔。

```bash
# 背景執行（建議）
nohup bash scripts/run_experiment.sh > results/experiment.log 2>&1 &
```

驗證是否執行結束：

```bash
# 無輸出 = 已結束
pgrep -f "run_experiment.sh" || echo "Experiment completed."
```

或檢查 log 尾端：

```bash
tail -10 results/experiment.log
```

完成後會在 `results/` 產生 `baseline-sample100_*.csv`、`gpu-sample100_*.csv`，並呼叫 `score.py` 輸出 PAR-2 比較。
