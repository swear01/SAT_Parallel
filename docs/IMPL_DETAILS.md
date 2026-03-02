# 實作細節

> 資料結構定義、公式、通訊協定 payload、GC 規則。
> 架構決策見 `RESEARCH_PLAN.md`，可調參數見 `config/default_params.yaml`。

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

**節點權重衰減：**
```
W_node(c)^{t+1} = γ · W_node(c)^{t} + Δ_W

Δ_W: 參與衝突 → +1, GPU hotzone → +GPU_BOOST
```

**Clause → Variable 聚合（Weighted Sum）：**
```
Score(v) = Σ α · Centrality(c) · W_node(c)   for c ∈ C_v
```

**與 VSIDS 混合：**
```
Final_Score(v) = (1 - λ) · VSIDS(v) + λ · GraphScore(v)
```

**Worker 權重融合：**
```
W_local(v) = α_local · W_local(v) + (1 - α_local) · W_global(v)
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

## 4. 通訊協定 Payload

### Delta Patch（Worker → Master）

```yaml
trigger:  # 任一條件
  - conflict_count_since_last >= delta_patch_conflict_interval
  - learnt_clause_with_lbd <= delta_patch_lbd_trigger
payload:
  high_quality_clauses:  # LBD ≤ lbd_entry_threshold
    - { clause_id, literals, lbd }
  conflict_pairs:
    - [clause_a, clause_b, delta_count]
  hot_variables:
    - [var_id, frequency]
transport: lock_free_queue (MPSC)
budget: "< 4KB per patch"
```

### Broadcast（Master → Workers）

```yaml
frequency: broadcast_interval_ms
payload:
  top_k_variable_scores:
    - [var_id, global_score]
  top_k_clause_weights:
    - [clause_id, global_weight]
  shared_learnt_clauses:  # LBD ≤ 2
transport: shared_memory
```

### GPU ↔ Master

```yaml
gpu_to_master:
  transport: CUDA_pinned_memory + lock_free_queue
  payload:
    hotzone_clause_ids: list[(clause_id, frequency)]
    best_assignment: dict[var_id, bool]
    unsat_count: int
  master_action:
    - hotzone nodes += GPU_BOOST weight
    - phase hints → broadcast to Workers

master_to_gpu:
  transport: CUDA_pinned_memory
  payload: new_learnt_clauses (LBD ≤ learnt_clause_lbd_filter)
  frequency: master_push_interval_s
```

---

## 5. 圖清理規則

### 節點淘汰

```yaml
entry_filter:
  original_clauses: 永遠保留
  learnt_clauses: LBD ≤ lbd_entry_threshold 才能進圖
eviction_conditions:  # 全部滿足時移除
  - weight < eviction_weight_threshold
  - lbd > eviction_lbd_threshold
  - age > min_age_before_eviction
frequency: 每 gc_interval 次 Master 更新
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

## 6. C++ 資料結構

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

struct DeltaPatch {
    uint32_t worker_id;
    uint64_t conflict_count;

    struct LearnedClause {
        uint32_t clause_id;
        std::vector<int> literals;
        int lbd;
    };
    std::vector<LearnedClause> new_clauses;

    struct ConflictPair {
        uint32_t clause_a, clause_b;
        int delta_count;
    };
    std::vector<ConflictPair> conflict_pairs;

    struct HotVariable {
        uint32_t var_id;
        int frequency;
    };
    std::vector<HotVariable> hot_variables;
};

struct GlobalBroadcast {
    uint64_t timestamp;

    struct VarScore { uint32_t var_id; float global_score; };
    std::vector<VarScore> top_k_var_scores;

    struct ClauseWeight { uint32_t clause_id; float global_weight; };
    std::vector<ClauseWeight> top_k_clause_weights;

    std::vector<std::vector<int>> shared_clauses;
};
```

### 圖切割參數

```yaml
partitioning:
  algorithm: louvain             # louvain | label_propagation
  target_communities: N_workers
  max_cut_variables: 15
  enable_work_stealing: true
```
