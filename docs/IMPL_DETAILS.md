# 實作細節

> 資料結構定義、公式、GC 規則。
> 架構決策見 `RESEARCH_PLAN.md`，可調參數見 `config/default_params.yaml`。
> DSRG 使用方式見 `docs/DSRG_USAGE_SUMMARY.md`。

> **架構說明：** 現行採用 Painless DSRGSharing（shr-strat=4），透過 CaDiCaL Tracer 取得 antecedents 建立 co-conflict 邊。GPU Prober 已整合（`-shr-gpu`），hotzone 回報納入 boost_node 流程。

---

## 1. DSRG 資料結構

### 節點

```yaml
node:
  id: clause_id        # uint32, 與 Clause DB 對齊
  attributes:
    length: int
    lbd: int
    activity: float64
    is_original: bool
    birth_conflict: int
  state:
    weight: float64
    community_id: int
```

### 邊

```yaml
edge:
  source: clause_id
  target: clause_id
  attributes:
    shared_variables: list[var_id]
    co_conflict_count: int
    weight: float64
  creation_rule: >
    只有當兩個 Clause 共同導致衝突超過 EDGE_THRESHOLD 次時才建立。
```

---

## 2. 權重更新公式

**邊權重：**
```
W_edge(c_i, c_j) = β · W_edge(c_i, c_j) + (1 - β) · Δ_conflict(c_i, c_j)
```

**節點權重衰減（Painless DSRGSharing）：**
```
W_node(c)^{t+1} = γ · W_node(c)^{t} + Δ_W

Δ_W: GPU hotzone 回報 → boost_node(clause_id, gpu_hotzone_boost × frequency)
（已實作，`-shr-gpu` 啟用時 drain reports 並 boost）

### Painless + DSRG+GPU 執行注意

- 直接執行：`./deps/painless/painless <cnf> -shr-strat=4 -shr-gpu -c=<n> -t=<timeout>`
- **建議 `-solver=kc`**（Kissat+CaDiCaL）當 `-c` > 8：預設 portfolio `kcl` 含 Lingeling，在大量 solver 時可能造成 heap 損壞
- DSRG tracer 在 `addInitialClauses` 期間會暫時停用，避免 CaDiCaL proof 與多執行緒競態
```

**Clause → Variable 聚合（Weighted Sum）：**
```
Score(v) = Σ α · Centrality(c) · W_node(c)   for c ∈ C_v
```

**與 VSIDS 混合：**
```
Final_Score(v) = (1 - λ) · VSIDS(v) + λ · GraphScore(v)
```

---

## 3. 中心性計算

**選項 A — 加權 Degree Centrality（先用）：**
```
Centrality(c) = Σ W_edge(c, c_j)   for all neighbors c_j
```

**選項 B — 近似 PageRank（進階）：**
```
Power Iteration, damping=d, ε, max_iter 見 config
```

---

## 4. GPU Hotzone → boost_node（已實作）

```yaml
來源: GPUProber 回報 (WalkSAT unsat 統計)
payload: GPUReport.hotzone = [(clause_id, frequency), ...]  # raw frequency
邏輯: DSRGSharing 在 runCentralityAndPhaseHints 中 drain reports
      主程式內由 frequency + DSRG 節點屬性（length、lbd）計算 weight
      再呼叫 dsrg_.boost_node(clause_id, weight)
時序: 先 boost，再 decay_all_weights，再接 centrality 計算
```

詳見 `docs/GPU_PROBER_INTEGRATION_PLAN.md`。

---

## 5. Co-conflict 邊（Painless 路徑）

在 `DSRGSharing::drainPendingClauses` 中，依據 CaDiCaL Tracer 提供的 antecedents 建立邊：

```yaml
來源: AntecedentProvider::getLastDerivation() → { ca_id, antecedents }
邏輯:
  - 新 clause 與每個 antecedent 之間: record_co_conflict
  - antecedent 兩兩之間: record_co_conflict
  - caIdToDsrgId_ 維持 ca_id → dsrg_id 映射，供後續 clause 參考
  - GC 時清理已淘汰節點對應的映射
```

---

## 6. 圖清理規則

### 節點淘汰

```yaml
entry_filter:
  original_clauses: 永遠保留
  learnt_clauses: LBD ≤ lbd_entry_threshold 才能進圖
eviction_conditions:  # 全部滿足時移除
  - weight < eviction_weight_threshold
  - lbd > eviction_lbd_threshold
  - age > min_age_before_eviction
frequency: DSRGSharing 定期 runCentralityAndPhaseHints 內 evict_stale_nodes
```

### 邊稀疏化

```yaml
creation: co_conflict_count >= edge_creation_threshold
removal: edge_weight < edge_removal_threshold
on_node_removal: 刪除所有連接邊
```

### In-processing 同步

```
Subsumption（A 涵蓋 B）：
  1. B 的所有 Edges 合併到 A（已存在則加權合併）
  2. B 的 weight 加到 A
  3. 刪除 B

Variable Elimination（變數 v 被消除）：
  1. 包含 v 的 Clause nodes 更新或移除
  2. 新 Resolvent clauses 若符合 LBD 門檻則加入圖
```

---

## 7. C++ 資料結構

```cpp
struct GraphNode {
    uint32_t clause_id;
    float    weight;
    int      lbd;
    int      length;
    bool     is_original;
    uint64_t birth_conflict;
    int      community_id;
};

struct GraphEdge {
    uint32_t source;
    uint32_t target;
    float    weight;
    int      co_conflict_count;
};

> DeltaPatch、GlobalBroadcast 等結構已隨 Master/Worker 架構移除。現行 DSRGSharing 使用 Painless 內建 clause sharing。

### Clause-Variable 雙向索引

DSRG 內部維護 clause-variable 的雙向映射，供中心性聚合使用：

```cpp
// DSRG 內部成員
clause_vars_: unordered_map<uint32_t, vector<uint32_t>>  // clause_id -> var_ids
var_clauses_: unordered_map<uint32_t, vector<uint32_t>>  // var_id -> clause_ids
```

- `add_node()` 接受 `literals` 參數，從中提取 `var_id = abs(literal)` 建立索引
- `remove_node()` 同步清理兩側索引
- `handle_variable_elimination()` 從 `var_clauses_` 找到受影響的 clauses

> 圖切割參數（Louvain、work stealing）已隨 Master 架構移除。
