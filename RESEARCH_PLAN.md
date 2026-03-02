# SAT-Parallel: DSRG + Multi-core Solver

> 動態語義解析圖 (Dynamic Semantic Resolution Graph) 驅動的多核心 SAT 求解器

---

## 1. 系統架構

三種角色透過非同步通訊協作：

- **Master (1 thread)** — 維護 Global DSRG、執行圖演算法（中心性 / 社群偵測）、生成 Cubes 分配任務、聚合回饋。不做 CDCL 搜尋。
- **Worker (N threads)** — 各自執行深層 CDCL、維護 Local Graph Weights 副本、定期回傳 Delta Patch、融合 Master 廣播的全域權重。不直接修改 Global DSRG。
- **GPU Prober** — 持續平行 WalkSAT，回傳 Hotzone Clause IDs 與 Phase Hints。不證明 UNSAT。

### 資料流

1. **GPU → Master：** Hotzone（最常卡住的 Clause IDs）+ Phase Hints（局部最佳賦值）
2. **Master → GPU：** 高品質 Learnt Clauses（LBD ≤ 3）
3. **Worker → Master：** Delta Patch（高品質 Clauses + 衝突對 + 熱變數）
4. **Master → Workers：** Top-K 全域權重 + 共享 Clauses + Cubes

---

## 2. DSRG（Dynamic Semantic Resolution Graph）

以 **Clause 為節點** 的動態圖，邊代表語義關聯而非靜態語法共享。

### 核心設計決策

- **節點 = Clause。** 每個節點追蹤 weight、LBD、所屬社群。Original clauses 永遠保留；Learnt clauses 須通過 LBD 門檻才能進圖。
- **邊 = 衝突共現。** 兩個 Clause 共同參與衝突解析超過閾值次數後才建立邊。邊權重反映語義關聯強度，非靜態共享變數。
- **權重動態衰減。** 節點與邊的權重隨時間指數衰減（類似 VSIDS），由新衝突事件與 GPU hotzone 回饋驅動增量。

---

## 3. 核心演算法

### 3.1 中心性 → 標定核心 Clauses

Master 定期在 DSRG 上執行中心性計算。先用 **加權 Degree Centrality**（最輕量），驗證有效後可嘗試 **近似 PageRank**。

### 3.2 Clause → Variable 聚合

對每個變數 v，將所有包含 v 的 Clause 的中心性分數加權聚合，產生變數優先級。首選 **Weighted Sum**（中心性 × 節點權重）。

### 3.3 與 VSIDS 混合

```
Final_Score(v) = (1 - λ) · VSIDS(v) + λ · GraphScore(v)
```

不完全取代 VSIDS，做加權混合。VSIDS 保底，GraphScore 提供語義引導。

---

## 4. 通訊機制

### 設計原則

- **極簡通訊：** Worker 不在每次 conflict 都回報，僅在滿足觸發條件時打包一個小型 Delta Patch（< 4KB）。
- **非同步無鎖：** 所有通訊走 lock-free queue（MPSC），Worker 與 GPU 永遠不被 Master 阻塞。
- **局部副本 + 定期同步：** 每個 Worker 維護自己的權重副本，Master 定期廣播全域 Top-K 權重，Worker 以指數移動平均融合。

### Delta Patch 內容（Worker → Master）

高品質 Learnt Clauses、頻繁共衝突的 Clause 對、頻繁衝突的變數。

### Broadcast 內容（Master → Workers）

Top-K 全域變數分數、Top-K Clause 權重、跨 Worker 共享的極品 Clauses。

---

## 5. 圖切割（Graph-based Partitioning）

利用 DSRG 的社群結構低成本指導多核心任務切割，取代傳統 Cube-and-Conquer 中昂貴的 Lookahead。

1. Master 在 DSRG 上執行 **社群偵測**（Louvain / Label Propagation）
2. 跨社群邊對應的 shared variables = **Cut Variables**
3. 實例化 Cut Variables → 生成 **Cubes** → 分配給 Workers
4. Worker 完成後 → **work stealing**（Master 再切割最大未解 Cube）

---

## 6. 圖清理（GC）

圖必須隨 Clause DB 同步成長與縮減，避免記憶體爆炸。

- **進入門檻：** Learnt clause 必須通過 LBD 門檻才能成為節點；邊必須累積足夠共衝突次數才建立。
- **淘汰機制：** 權重衰減至閾值以下且 LBD 偏高的節點被移除；弱邊定期清除。
- **In-processing 同步：** Subsumption 發生時合併節點與邊；Variable Elimination 時同步移除相關節點。

---

## 7. 實作平台

**Painless 框架 + CaDiCaL 引擎**
- Painless 已內建 Master-Worker 通訊抽象與 clause sharing 機制，且已介接 CaDiCaL 作為底層 solver
- 改裝：在 Painless 的 sharing strategy 層加入 DSRG 模組、將一個 Worker slot 改為 Master 角色、新增 GPU Prober 模組

---

## 8. 檔案結構

```
SAT_Parallel/
├── RESEARCH_PLAN.md              # 架構決策（本文件）
├── docs/
│   └── IMPL_DETAILS.md           # 資料結構、公式、參數細節
├── config/
│   └── default_params.yaml       # 所有可調參數
├── references.bib
├── src/
│   ├── core/                     # DSRG、中心性、聚合、GC
│   ├── master/                   # Master 邏輯、社群偵測、廣播
│   ├── worker/                   # Worker 邏輯、局部權重、Delta Patch
│   ├── gpu/                      # CUDA WalkSAT、GPU Prober
│   ├── comm/                     # lock-free queue、shared memory
│   └── solver/                   # Painless 框架 + CaDiCaL
├── scripts/                      # benchmark / ablation 腳本
└── benchmarks/
```

---

## 9. 參考文獻

| 文獻 | 用途 |
|---|---|
| **NeuroSAT** — Selsam et al., ICLR 2019 | 圖結構靈感（動態語義邊取代靜態語法邊） |
| **TurboSAT** — Ozolins et al., 2022 | GPU 加速可行性（改為持續探測） |
| **Painless** — Le Frioux et al., SAT 2017 | 實作平台（平行框架，已介接 CaDiCaL） |
| **CaDiCaL** — Biere et al., SAT Competition 2020 | Worker 引擎（經由 Painless 介接） |
| **Cube-and-Conquer** — Heule et al., HVC 2012 | 用圖社群偵測取代 Lookahead 切割 |
| **WalkSAT** — Selman et al., AAAI 1994 | GPU 探測演算法基礎 |
| **Louvain** — Blondel et al., JSTAT 2008 | 社群偵測演算法 |
| **PageRank** — Page et al., Stanford 1999 | 中心性計算候選 |
| **Parallel SAT Survey** — Balyo & Sinz, JSAT 2018 | 平行 SAT 全景 |
