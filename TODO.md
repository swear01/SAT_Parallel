# TODO List

> 按階段推進，每個階段有明確的驗收標準。

---

## Phase 0 — 測試計分框架 & Baseline 驗證

> **目標：** 建立可重複、可自動化的 benchmark 測試流程，跑出 vanilla Painless 的 baseline 成績作為後續改進的比較基準。

- [ ] **0.1** 取得 SAT Competition benchmark 測試集
  - 下載 SAT Competition 2023/2024 的 main track benchmark instances
  - 依 SAT / UNSAT / UNKNOWN 分類放入 `benchmarks/`
  - 建立 `benchmarks/instances.csv` 索引（路徑、預期結果、類別）

- [ ] **0.2** 撰寫 benchmark runner (`scripts/run_benchmark.py`)
  - 輸入：solver binary path、benchmark 目錄、timeout（預設 5000s）、核心數
  - 對每個 instance 執行 solver，記錄 wall time、exit code、結果（SAT/UNSAT/TIMEOUT）
  - 輸出：`results/<solver>_<timestamp>.csv`（每列一個 instance）
  - 支援平行跑多個 instance（善用 32 核）

- [ ] **0.3** 撰寫計分與分析工具 (`scripts/score.py`)
  - 從 results CSV 計算：
    - **Solved count**（SAT 解數、UNSAT 解數、總解數）
    - **PAR-2 score**（Penalized Average Runtime：未解 = 2 × timeout）
    - **Median / Mean solve time**（僅已解 instances）
    - **Cactus plot**（X 軸 = 解題數量排名，Y 軸 = 累積時間）
  - 支援多 solver 結果疊加比較
  - 輸出文字摘要 + 圖表 PNG

- [ ] **0.4** 跑 Baseline：vanilla Painless + CaDiCaL
  - 用 `deps/painless/painless` 跑完整 benchmark set
  - 記錄不同核心數（1, 4, 8, 16, 32）的成績
  - 保存結果至 `results/baseline/`

- [ ] **0.5** 跑 Baseline：standalone CaDiCaL（單核對照組）
  - 用 `deps/cadical/build/cadical` 跑相同 benchmark
  - 作為單核 sequential solver 基準線

- [ ] **0.6** 撰寫 baseline 報告
  - 整理各配置的 PAR-2 / solved count 表格
  - 繪製 cactus plot 比較圖
  - 存入 `docs/baseline_report.md`

---

## Phase 1 — DSRG 核心資料結構

> **目標：** 實作 DSRG 圖的基本結構與操作，能獨立單元測試。

- [ ] **1.1** 實作 `GraphNode` / `GraphEdge` 結構體 (`src/core/dsrg_types.h`)
- [ ] **1.2** 實作 DSRG 圖類別 (`src/core/dsrg.h`, `src/core/dsrg.cpp`)
  - 新增/移除節點與邊
  - 權重更新（指數衰減 + 衝突驅動增量）
  - 邊的建立門檻判定
- [ ] **1.3** 實作 GC 機制 (`src/core/dsrg_gc.cpp`)
  - 節點淘汰（weight < threshold && lbd > threshold && age > min）
  - 弱邊清除
  - In-processing 同步（subsumption merge、variable elimination）
- [ ] **1.4** 撰寫 DSRG 單元測試
- [ ] **1.5** 基準效能測試：10 萬節點 / 50 萬邊的增刪與查詢延遲

---

## Phase 2 — 中心性 & 變數聚合

> **目標：** 從 DSRG 計算出變數分數，可與 VSIDS 混合。

- [ ] **2.1** 加權 Degree Centrality (`src/core/centrality.cpp`)
- [ ] **2.2** 近似 PageRank（選項 B，進階）
- [ ] **2.3** Clause → Variable 聚合 (`src/core/aggregation.cpp`)
  - Weighted Sum: `Score(v) = Σ α · Centrality(c) · W_node(c)`
- [ ] **2.4** VSIDS 混合公式
  - `Final_Score(v) = (1 - λ) · VSIDS(v) + λ · GraphScore(v)`
- [ ] **2.5** 單元測試 + 正確性驗證

---

## Phase 3 — 通訊層

> **目標：** 建立 Master / Worker / GPU 之間的非同步通訊基礎。

- [ ] **3.1** Lock-free MPSC queue (`src/comm/mpsc_queue.h`)
- [ ] **3.2** Delta Patch 結構與序列化 (`src/comm/delta_patch.h`)
- [ ] **3.3** GlobalBroadcast 結構與共享記憶體傳輸 (`src/comm/broadcast.h`)
- [ ] **3.4** GPU pinned memory 通訊介面 (`src/comm/gpu_channel.h`)
- [ ] **3.5** 通訊層壓力測試（throughput / latency）

---

## Phase 4 — Master 模組

> **目標：** 實作 Master 角色邏輯，整合進 Painless 框架。

- [ ] **4.1** Master 主迴圈 (`src/master/master.cpp`)
  - 接收 Delta Patch → 更新 DSRG
  - 定期執行中心性計算
  - 廣播 Top-K 權重 + 共享 clauses
- [ ] **4.2** 社群偵測：Louvain (`src/master/community.cpp`)
- [ ] **4.3** 圖切割 → Cube 生成 (`src/master/partitioner.cpp`)
  - 跨社群邊 → cut variables → cube 分配
- [ ] **4.4** Work stealing 邏輯
- [ ] **4.5** 整合至 Painless sharing strategy 層

---

## Phase 5 — Worker 模組

> **目標：** 修改 Painless Worker，使其能與 Master 協作。

- [ ] **5.1** Worker 局部權重維護 (`src/worker/local_weights.cpp`)
- [ ] **5.2** Delta Patch 生成邏輯（觸發條件、打包）
- [ ] **5.3** 接收 Broadcast → 融合全域權重（EMA）
- [ ] **5.4** 接收 Cube 分配 → 加入假設文字
- [ ] **5.5** 整合至 Painless worker 引擎

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
