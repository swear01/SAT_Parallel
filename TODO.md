# TODO List

> 按階段推進，每個階段有明確的驗收標準。

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

## Phase 3 — 通訊層

> **目標：** 建立 Master / Worker / GPU 之間的非同步通訊基礎。

- [x] **3.1** Lock-free MPSC queue (`src/comm/mpsc_queue.h`) — Vyukov wait-free push / lock-free pop
- [x] **3.2** Delta Patch 結構與序列化 (`src/comm/delta_patch.h`) — 含 estimated_size_bytes() budget 檢查
- [x] **3.3** GlobalBroadcast + BroadcastChannel (`src/comm/broadcast.h`) — mutex-guarded shared_ptr (GCC 11)
- [x] **3.4** GPU pinned memory 通訊介面 (`src/comm/gpu_channel.h`) — MPSC (GPU→Master) + single-slot (Master→GPU)
- [x] **3.5** 通訊層單元測試 + 壓力測試 — 13 tests PASS, MPSC 26.5 M ops/s

---

## Phase 4 — Master 模組

> **目標：** 實作 Master 角色邏輯，整合進 Painless 框架。

- [x] **4.1** Master 主迴圈 (`src/master/master.cpp`) — tick() 驅動：drain DeltaPatch + GPU report → 定期 centrality + GC + broadcast
- [x] **4.2** 社群偵測：Louvain + Label Propagation (`src/master/community.cpp`) — 含 modularity gain 優化、compact community IDs
- [x] **4.3** 圖切割 → Cube 生成 (`src/master/partitioner.cpp`) — cut variable 權重排序 → 2^k cubes → round-robin 分配
- [x] **4.4** Work stealing (`src/master/work_stealing.h`) — thread-safe cube 管理、SAT 早停
- [x] **4.5** Painless sharing strategy 介面 (`src/master/sharing_strategy.h`) — DSRGSharingStrategy 封裝完整 pipeline
- [x] **4.6** 單元測試 — 12 tests PASS（Master tick / community / partitioner / work stealing / e2e）

---

## Phase 5 — Worker 模組

> **目標：** 修改 Painless Worker，使其能與 Master 協作。

- [x] **5.1** Worker 局部權重維護 (`src/worker/local_weights.h`) — EMA 融合 + VSIDS 混合 + top-K 選取
- [x] **5.2** Delta Patch 生成邏輯 (`src/worker/patch_builder.h`) — conflict interval / LBD 雙觸發、budget 裁剪 < 4KB
- [x] **5.3** 接收 Broadcast → 融合全域權重 — `merge_broadcast()` EMA: `W_local = α·W_local + (1-α)·W_global`
- [x] **5.4** 接收 Cube 分配 → 假設文字轉換 — `set_cube()` → assumption literals
- [x] **5.5** Worker 引擎整合 (`src/worker/worker_engine.h`) — SolverInterface 抽象、自動 broadcast polling、16 tests PASS

---

## Phase 6 — GPU Prober

> **目標：** 在 GPU 上持續執行 WalkSAT，回傳 hotzone 與 phase hints。

- [ ] **6.1** CUDA WalkSAT kernel (`src/gpu/walksat_kernel.cu`)
- [ ] **6.2** GPU clause DB 管理（pinned memory 傳入新 learnt clauses）
- [ ] **6.3** Hotzone 回報（最常卡住的 clause IDs + 頻率）
- [ ] **6.4** Phase Hints 回報（局部最佳賦值）
- [ ] **6.5** GPU ↔ Master 整合測試

---

## Phase 7 — 端對端整合 & 調參

> **目標：** 完整系統跑通，進行效能比較與消融實驗。

- [ ] **7.1** 端對端整合：Master + Workers + GPU Prober
- [ ] **7.2** 用 Phase 0 的 benchmark 框架跑完整測試
- [ ] **7.3** 與 baseline 比較（PAR-2、solved count、cactus plot）
- [ ] **7.4** 消融實驗（ablation study）
  - 開/關 DSRG 中心性引導
  - 開/關 GPU Prober
  - 開/關圖切割 vs 傳統 Cube-and-Conquer
  - 調整 λ、decay rate、broadcast interval 等參數
- [ ] **7.5** 撰寫最終報告

---

## 標記說明

- `[ ]` 待開始
- `[~]` 進行中
- `[x]` 已完成
