# DSRG 使用方式與權重更新整理

## DSRG 使用場景

### Painless DSRGSharing（shr-strat=4，benchmark 用）

**位置：** `deps/painless/src/sharing/LocalStrategies/DSRGSharing.cpp`

**流程：**

```
Producer (Cadical) learn clause
    → exportClause → DSRGSharing::importClause
    → LBD 過濾：clause->lbd <= dsrgLbdThreshold 才進 DSRG
    → pendingClauses_.push_back({dsrg_id, lits, lbd})
    → doSharing() 每 round：
        drainPendingClauses() → dsrg_.add_node(dsrg_id, lits, lbd, false, round_)
        定期 runCentralityAndPhaseHints()
        定期 evict_stale_nodes + prune_weak_edges
```

**權重相關：**

| 操作 | 是否呼叫 | 說明 |
|------|----------|------|
| `add_node` | ✓ | 新節點 weight 初始化為 1.0 |
| `record_co_conflict` | ✓ | 透過 antecedents 建立 co-conflict 邊（CaDiCaL Tracer） |
| `boost_node` | ✓ | GPU hotzone 驅動（`-shr-gpu` 啟用時） |
| `decay_all_weights` | ✓ | 僅在 `runCentralityAndPhaseHints()` 內呼叫 |

**權重實際行為：**

- 新節點：`weight = 1.0`
- 每 `centralityIntervalRounds` 呼叫 `decay_all_weights`：`node.weight *= gamma`（預設 0.95）
- `record_co_conflict` 建立 clause 間 co-conflict 邊，供 centrality 使用

**中心性與 phase：**

1. `decay_all_weights()` 後
2. `compute_centrality(dsrg_, cfg)` → 依 `adj_` 的邊權重算（co-conflict 邊已實作）
3. `aggregate_to_variables()` → `Score(v) = Σ α * centrality(c) * node.weight`（依 clause-variable 索引）
4. `select_top_k(varScores_, phaseTopK)` → 選前 K 個變數
5. `solver->setPhase(var_id, score > 0)` 傳給 CDCL solvers

**註：** 獨立 sat_parallel（master/worker）已棄用。GPU Prober 已整合至 DSRGSharing，hotzone 回報納入 `boost_node` 流程。詳見 `docs/GPU_PROBER_INTEGRATION_PLAN.md`。

---

## 權重更新公式（設計規格）

依 `docs/IMPL_DETAILS.md`：

```
W_edge(c_i, c_j) = β · W_edge + (1 - β) · Δ_conflict
W_node^{t+1}     = γ · W_node + Δ_W    (Δ_W: 衝突 +1 / GPU hotzone + boost)
Score(v)         = Σ α · Centrality(c) · W_node(c)   for c ∈ C_v
```

**Painless DSRGSharing 現況：**

- ✓ `record_co_conflict` 已透過 antecedents 實作 co-conflict 邊
- ✓ `boost_node`：GPU hotzone 回報經 `-shr-gpu` 啟用時，drain reports 後依 frequency 正規化計算權重並 boost
- 中心性使用 co-conflict 邊權重，phase 來自 centrality 與 clause-variable 重疊

---

## co-conflict 邊實作狀態

已透過 CaDiCaL Tracer 取得 antecedents，在 `importClause` / `drainPendingClauses` 時呼叫 `record_co_conflict` 建立與更新 co-conflict 邊，`compute_centrality` 現可使用非零邊權重。
