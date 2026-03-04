# SAT-Parallel: DSRG + Multi-core Solver

> 動態語義解析圖 (Dynamic Semantic Resolution Graph) 驅動的多核心 SAT 求解器

---

## 1. 系統架構（現行）

**Painless 框架 + DSRGSharing（shr-strat=4）**

- **Painless** 負責平行 CDCL、clause sharing、負載平衡
- **DSRGSharing** 作為 sharing strategy：接收各 Producer 的 learnt clauses，維護 DSRG 圖，定期計算中心性與 phase hints
- **DSRG 圖** 以 clause 為節點、co-conflict 為邊；透過 CaDiCaL Tracer 取得 antecedents 建立邊
- **中心性與 phase** 經由 `compute_centrality` → `aggregate_to_variables` → `select_top_k` → `setPhase` 引導 CDCL 決策

### 資料流

1. **Producer (Cadical)** learn clause → `exportClause` → `DSRGSharing::importClause`
2. **AntecedentProvider**（透過 CaDiCaL Tracer）提供 antecedents → `record_co_conflict` 建立邊
3. **DSRGSharing** 定期 `drainPendingClauses`、`runCentralityAndPhaseHints`、GC
4. **Phase hints** 回傳給各 CDCL solver 引導變數賦值順序

### GPU Prober（已整合）

GPU WalkSAT probing 已整合至 DSRGSharing 權重更新流程（`-shr-strat=4 -shr-gpu`）：

- **GPUProber** 在獨立 thread 執行 WalkSAT，統計各 clause 的 unsat 次數
- 回報 **hotzone**（top-K 高 unsat 次數的 clause_id）透過 GPUChannel 送至 DSRGSharing
- DSRGSharing 在 `runCentralityAndPhaseHints` 中 drain reports，呼叫 `boost_node(clause_id, gpu_hotzone_boost × frequency)`
- **動態 launch 配置：** 依 GPU 裝置屬性（SM 數量、可用記憶體）自動計算 grid/block，不再寫死 64 threads；可經 `config.num_walks` 或 YAML `gpu.num_walks` 設定最少 walks
- 詳見 [docs/GPU_PROBER_INTEGRATION_PLAN.md](docs/GPU_PROBER_INTEGRATION_PLAN.md)

### 已棄用架構

原先的 Master / Worker 架構（Delta Patch、Broadcast、社群偵測、圖切割）已移除，詳見 [TODO.md](TODO.md) 已棄用架構一節。

---

## 2. DSRG（Dynamic Semantic Resolution Graph）

以 **Clause 為節點** 的動態圖，邊代表語義關聯而非靜態語法共享。

### 核心設計決策

- **節點 = Clause。** 每個節點追蹤 weight、LBD。Original clauses 永遠保留；Learnt clauses 須通過 LBD 門檻才能進圖。
- **邊 = 衝突共現（co-conflict）。** 兩個 Clause 共同參與同一 conflict 的 resolution 則建立邊（透過 CaDiCaL antecedents）。邊權重反映語義關聯強度。
- **權重動態衰減。** 節點與邊的權重隨時間指數衰減（類似 VSIDS）。**GPU hotzone boost** 作為增量輸入，`boost_node(clause_id, Δ)` 對應 WalkSAT 回報的 high-unsat clauses。
- **Clause-Variable 雙向索引。** DSRG 在新增節點時記錄 clause 包含的 variable IDs，維護 clause→vars 與 var→clauses 雙向映射，供 Phase 2 聚合計算使用。

### 實作決策

- **權重衰減策略：全量掃描。** 定期遍歷所有節點（`w *= γ`）與邊（`w *= β`），與增量（`boost_node`）分離操作。100K nodes + 500K edges 的全量掃描 < 1ms，不在 solver 熱路徑上。未來可切換為 lazy decay（VSIDS 風格），介面不變。
- **Adjacency list 用 `vector` + swap-and-pop。** 相比 `unordered_set`，遍歷鄰居更 cache-friendly（中心性計算的熱路徑），O(degree) 的移除在平均度數 < 20 下可接受。
- **Pending conflicts lazy cleanup。** 節點刪除時不掃描 `pending_conflicts_`，建邊時才檢查兩端節點是否存在。
- **不使用外部圖庫。** DSRG 的操作高度領域特定（權重衰減、co-conflict 追蹤、GC 淘汰、subsumption merge），通用圖庫（Graaf、CXXGraph、BGL）無法直接支援，反而增加抽象開銷與記憶體佈局限制。

---

## 3. 核心演算法

### 3.1 中心性 → 標定核心 Clauses

DSRGSharing 定期在 DSRG 上執行中心性計算。先用 **加權 Degree Centrality**（最輕量），驗證有效後可嘗試 **近似 PageRank**。

### 3.2 Clause → Variable 聚合

對每個變數 v，將所有包含 v 的 Clause 的中心性分數加權聚合，產生變數優先級。首選 **Weighted Sum**（中心性 × 節點權重）。

### 3.3 與 VSIDS 混合

```
Final_Score(v) = (1 - λ) · VSIDS(v) + λ · GraphScore(v)
```

不完全取代 VSIDS，做加權混合。VSIDS 保底，GraphScore 提供語義引導。

---

## 4. 通訊機制

- **Clause sharing** 由 Painless 的 sharing pipeline 處理，DSRGSharing 在 `importClause` 接收 clauses
- **Co-conflict 邊** 透過 CaDiCaL Tracer 取得的 antecedents 建立，不依賴外部 Delta Patch
- **Phase hints** 經 DSRGSharing 的 `runCentralityAndPhaseHints` 計算後傳回各 solver
- **GPU hotzone** 經 GPUChannel（MPSC queue）非同步回傳，DSRGSharing drain 後呼叫 `boost_node`

---

## 5. 圖切割（已棄用）

原先設計的圖切割（Louvain 社群偵測 → Cut Variables → Cubes → work stealing）已隨 Master/Worker 架構移除。現行架構僅用 DSRG 做中心性計算與 phase 引導，不做任務切割。

---

## 6. 圖清理（GC）

圖必須隨 Clause DB 同步成長與縮減，避免記憶體爆炸。

- **進入門檻：** Learnt clause 必須通過 LBD 門檻才能成為節點；邊必須累積足夠共衝突次數才建立。
- **淘汰機制：** 權重衰減至閾值以下且 LBD 偏高的節點被移除；弱邊定期清除。
- **In-processing 同步：** Subsumption 發生時合併節點與邊；Variable Elimination 時同步移除相關節點。

---

## 7. 實作平台

**Painless 框架 + CaDiCaL 引擎 + DSRGSharing**
- Painless 內建 clause sharing 與 CDCL 平行化
- DSRGSharing 作為 sharing strategy（shr-strat=4），使用 `src/core/` 的 DSRG、centrality、aggregation
- CaDiCaL 透過 DSRGCadicalTracer 提供 antecedents，供 `record_co_conflict` 建立 co-conflict 邊

---

## 8. 檔案結構

```
SAT_Parallel/
├── RESEARCH_PLAN.md              # 架構決策（本文件）
├── TODO.md                       # 階段任務與進度
├── docs/
│   ├── IMPL_DETAILS.md           # 資料結構、公式、參數細節
│   ├── DSRG_USAGE_SUMMARY.md     # DSRG 使用方式與權重更新整理
│   └── GPU_PROBER_INTEGRATION_PLAN.md  # GPU 整合設計
├── config/
│   └── default_params.yaml       # DSRG / centrality / gpu 可調參數
├── src/
│   ├── core/                     # DSRG、中心性、聚合、GC（Painless 連結）
│   ├── comm/                     # mpsc_queue、gpu_channel（GPU 用）
│   └── gpu/                      # GPUProber、WalkSAT kernel（選填）
├── deps/
│   └── painless/                 # Painless + DSRGSharing + DSRGCadicalTracer
├── scripts/                      # benchmark / 分析腳本
└── benchmarks/                   # SAT Competition 測試集
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
