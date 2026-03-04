# TODO List

> 按階段推進，每個階段有明確的驗收標準。

> **架構變更（2025）：** 獨立 sat_parallel（master/worker/gpu/comm）已棄用並移除。現僅保留 **Painless DSRGSharing** 整合路徑：DSRG 作為 sharing strategy（shr-strat=4），透過 CaDiCaL Tracer 取得 antecedents 建立 co-conflict 邊。benchmark 使用 `deps/painless/painless`。

---

## Phase 0 — 測試計分框架 & Baseline 驗證

> **目標：** 建立可重複、可自動化的 benchmark 測試流程，跑出 vanilla Painless 的 baseline 成績作為後續改進的比較基準。

> **硬體：** i9-13900K (24 核 / 32 緒)、NVIDIA RTX 4090

> **Preset 設定：**
>
> | Preset | Timeout | Instances | 用途 |
> |--------|---------|-----------|------|
> | `test` | 5s | 前 10 個 | 煙霧測試、CI 驗證 |
> | `full` | 1000s | 全部 400 個 | 正式 baseline 評估 |

- [x] **0.1** 取得 SAT Competition benchmark 測試集
  - SC2023 main track：400 instances（SAT=154, UNSAT=219, unknown=27）→ `benchmarks/sc2023/`
  - SC2024 main track：400 instances（SAT=179, UNSAT=208, unknown=13）→ `benchmarks/sc2024/`
  - 各年度獨立目錄，各自 `instances.csv`，不合併去重
  - 平行下載器 `scripts/download_benchmarks.py`：8 workers、背景模式、內嵌 SC2023/SC2024 URL

- [x] **0.2** 撰寫 benchmark runner (`scripts/run_benchmark.py`)
  - 支援 `--preset test`（5s × 10 instances）與 `--preset full`（1000s × all）
  - 自動偵測 solver 類型（Painless vs CaDiCaL），生成正確的 CLI 參數
  - 輸出：`results/<tag>_<timestamp>.csv`
  - `--solver-cpus 0` 自動使用全部核心

- [x] **0.3** 撰寫計分與分析工具 (`scripts/score.py`)
  - Solved count、PAR-2 score、Median/Mean solve time、Cactus plot
  - 支援多 solver 結果疊加比較
  - 輸出文字摘要 + 圖表 PNG

- [x] **0.4** 煙霧測試驗證（`--preset test`）
  - CaDiCaL 單核：4/10 solved（5s timeout）✓
  - Painless 32 緒：7/10 solved（5s timeout）✓

- [ ] **0.5** 跑 Baseline：full preset（`--preset full`，400 instances × 1000s，兩年分開跑）
  - SC2023: `--preset full --benchmarks benchmarks/sc2023/instances.csv --solver deps/cadical/build/cadical --tag cadical_sc2023`
  - SC2024: `--preset full --benchmarks benchmarks/sc2024/instances.csv --solver deps/cadical/build/cadical --tag cadical_sc2024`
  - Painless 32 緒同理
  - 保存結果至 `results/baseline/`
  - 預估耗時：數小時至數十小時

- [ ] **0.6** 撰寫 baseline 報告
  - 整理各配置的 PAR-2 / solved count 表格
  - 繪製 cactus plot 比較圖
  - 存入 `docs/baseline_report.md`

---

## Phase 1 — DSRG 核心資料結構

> **目標：** 實作 DSRG 圖的基本結構與操作，能獨立單元測試。
>
> **實作決策：** 全量掃描衰減（< 1ms @100K nodes）、vector + swap-and-pop adjacency list、
> Clause-Variable 雙向索引內建於 DSRG、不使用外部圖庫、C++20。

- [x] **1.1** 實作 `GraphNode` / `GraphEdge` / `DSRGConfig` 結構體 (`src/core/dsrg_types.h`)
  - `DSRGConfig` 從 `config/default_params.yaml` 載入
- [x] **1.2** 實作 DSRG 圖類別 (`src/core/dsrg.h`, `src/core/dsrg.cpp`)
  - 新增/移除節點與邊
  - 權重更新（全量掃描衰減 + 衝突驅動增量 + boost）
  - 邊的建立門檻判定（co-conflict count >= threshold）
  - Clause-Variable 雙向索引（clause_vars_ / var_clauses_）
- [x] **1.3** 實作 GC 機制 (`src/core/dsrg_gc.cpp`)
  - 節點淘汰（weight < threshold && lbd > threshold && age > min）
  - 弱邊清除（swap-and-pop 從 adjacency list 移除）
  - In-processing 同步（subsumption merge、variable elimination）
- [x] **1.4** 撰寫 DSRG 單元測試 (`tests/core/dsrg_test.cpp`, Google Test) — 23 tests PASS
- [x] **1.5** 基準效能測試：10 萬節點 / 50 萬邊的增刪與查詢延遲 (`tests/core/dsrg_bench.cpp`)

---

## Phase 2 — 中心性 & 變數聚合

> **目標：** 從 DSRG 計算出變數分數，可與 VSIDS 混合。

- [x] **2.1** 加權 Degree Centrality (`src/core/centrality.h`, `src/core/centrality.cpp`)
- [x] **2.2** 近似 PageRank（Power Iteration, damping/epsilon/max_iter 從 YAML 載入）
- [x] **2.3** Clause → Variable 聚合 (`src/core/aggregation.h`, `src/core/aggregation.cpp`)
  - 支援 weighted_sum / sum / max 三種方法
  - Top-K 變數選取（`select_top_k`）
- [x] **2.4** VSIDS 混合公式（`mix_with_vsids`）
  - `Final_Score(v) = (1 - λ) · VSIDS(v) + λ · GraphScore(v)`
- [x] **2.5** 單元測試 + 正確性驗證 (`tests/core/centrality_test.cpp`) — 25 tests PASS

---

## Phase 3 — Co-conflict 邊（Painless 整合）

> **目標：** 在 Painless DSRGSharing 中建立 co-conflict 邊，使中心性計算有效。

- [x] **3.1** AntecedentProvider 介面 (`deps/painless/src/solvers/CDCL/AntecedentProvider.hpp`) — 提供 `getLastDerivation()`
- [x] **3.2** DSRGCadicalTracer (`DSRGCadicalTracer.hpp/cpp`) — 繼承 CaDiCaL Tracer，在 `add_derived_clause` 時記錄 antecedents
- [x] **3.3** Cadical 整合 — 建構時 `connect_proof_tracer(dsrgTracer_)`，實作 `getLastDerivation()`
- [x] **3.4** DSRGSharing 使用 antecedents — `importClause` 取得 derivation，`drainPendingClauses` 呼叫 `record_co_conflict`，維護 `caIdToDsrgId_`
- [x] **3.5** GC 時清理 `caIdToDsrgId_` 已淘汰節點

---

## Phase 4 — GPU Prober 整合（規劃）

> **目標：** 恢復 GPU probing 架構，將其 hotzone 回報納入 DSRGSharing 權重更新流程。詳見 `docs/GPU_PROBER_INTEGRATION_PLAN.md`。

- [ ] **4.1** 還原 GPU 相關程式碼（gpu/, comm/mpsc_queue, comm/gpu_channel）
- [ ] **4.2** DSRGSharing 維護 `dsrgIdToLiterals_`，於 drain/GC 時同步
- [ ] **4.3** 主程式內由 frequency 計算權重（正規化 / 長度加權 / 純縮放，可調）
- [ ] **4.4** DSRGSharing 整合：drain hotzone reports → `boost_node(clause_id, weight)`；週期性 push clauses 至 GPU
- [ ] **4.5** Painless 參數 `-shr-gpu` 與 GPUProber 生命週期管理
- [ ] **4.6** Benchmark 開/關 GPU prober 比較

---

## Phase 5 — 端對端整合 & 調參（Painless 路徑）

> **目標：** 用 Painless + DSRGSharing 跑完整 benchmark，與 baseline 比較並調參。

- [ ] **5.1** 跑 Baseline：Phase 0 full preset（SC2023/SC2024 各 400 instances × 1000s）
- [ ] **5.2** 與 baseline 比較（PAR-2、solved count、cactus plot）
- [ ] **5.3** 消融實驗（ablation study）
  - 開/關 DSRG 中心性引導（shr-strat=4 vs vanilla Painless）
  - 調整 `dsrgLbdThreshold`、`centralityIntervalRounds`、decay rate 等參數
- [ ] **5.4** 撰寫 baseline / 改進報告

---

## 已棄用架構（程式碼已移除）

下列 Phase 曾實作並通過測試，但架構決策改為 **僅保留 Painless 整合路徑** 後已移除程式碼，僅作歷史紀錄：

- **原 Phase 3** — 通訊層（MPSC queue、Delta Patch、Broadcast、GPU channel）
- **原 Phase 4** — Master（社群偵測、圖切割、work stealing、sharing strategy）
- **原 Phase 5** — Worker（local weights、patch builder、broadcast 融合）
- **原 Phase 6** — GPU Prober（已規劃以整合方式恢復，見 Phase 4）

---

## 標記說明

- `[ ]` 待開始
- `[~]` 進行中
- `[x]` 已完成
