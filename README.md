# SAT-Parallel

> DSRG (Dynamic Semantic Resolution Graph) 驅動的多核心平行 SAT 求解器

---

## 系統需求

| 項目 | 最低需求 |
|------|----------|
| OS | Linux (Ubuntu 20.04+) |
| GCC/G++ | 11+ (需支援 C++20) |
| CMake | 3.18+ |
| CUDA Toolkit | 11.5+（建議 12.4 以完整支援 RTX 4090） |
| GPU | NVIDIA，Compute Capability 8.0+（設計目標為 RTX 4090, CC 8.9） |
| Python | 3.10+ |
| 磁碟空間 | ~5 GB（含 deps 建置） |

> **不需要 root/sudo 權限**，所有相依套件皆安裝於專案本地。

---

## 快速開始

### 1. 克隆專案

```bash
git clone <repo-url> SAT_Parallel
cd SAT_Parallel
```

### 2. 一鍵安裝環境

```bash
bash scripts/setup_env.sh
```

安裝腳本會自動完成以下工作：

| 步驟 | 說明 |
|------|------|
| 1 | 驗證系統工具（gcc, cmake, git, GPU） |
| 2 | 安裝 CUDA 12.4 toolkit 至 `~/.local/cuda-12.4`（本地，不需 sudo） |
| 3 | 編譯安裝 OpenMPI 5.0.6 至 `deps/local/openmpi/` |
| 4 | 克隆並編譯 CaDiCaL 求解器 |
| 5 | 克隆並編譯 Painless 平行框架（含 10 個子求解器） |
| 6 | 編譯安裝 yaml-cpp 0.8.0 |
| 7 | 建立 Python 虛擬環境並安裝分析套件 |
| 8 | 生成 `env.sh` 環境設定檔 |

腳本可重複執行，已完成的步驟會自動跳過。

若要跳過 CUDA 12 下載（約 4.5 GB），可使用：

```bash
bash scripts/setup_env.sh --skip-cuda
```

此模式下會使用系統 CUDA 11.5 搭配 sm_86 PTX forward compatibility（運行時 JIT 編譯至 sm_89）。

### 3. 載入環境

每次開啟新的終端機都需要執行：

```bash
source env.sh
```

### 4. 建置專案

```bash
source env.sh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j8
```

Debug 模式：

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j8
```

---

## Benchmark 測試

### 下載測試集

自動從 benchmark-database.de 平行下載 SAT Competition 2023/2024 Main Track 測試集。

```bash
source env.sh

# 下載 SC2023 + SC2024（796 instances，8 執行緒，約 20 分鐘）
python scripts/download_benchmarks.py

# 只下載特定年份
python scripts/download_benchmarks.py --year 2023
python scripts/download_benchmarks.py --year 2024

# 背景下載（適合 SSH 連線）
python scripts/download_benchmarks.py --bg
# Monitor: tail -f benchmarks/download.log

# 更多選項
python scripts/download_benchmarks.py -j 16          # 16 並行下載
python scripts/download_benchmarks.py --limit 50     # 每年只下載前 50 個
```

下載完成後自動產生 `benchmarks/instances.csv` 索引。再次執行會跳過已下載的檔案。

### 執行 Solver

兩種預設模式：

| Preset | Timeout | Instances | 用途 |
|--------|---------|-----------|------|
| `test` | 5s | 前 10 個 | 快速煙霧測試、確認 solver 可執行 |
| `full` | 1000s | 全部（796 個） | 正式 baseline 評估 |

```bash
# 煙霧測試（約 1 分鐘）
python scripts/run_benchmark.py --preset test --solver deps/cadical/build/cadical --tag cadical_test
python scripts/run_benchmark.py --preset test --solver deps/painless/painless --tag painless_test

# 正式執行（可能需要數小時）
python scripts/run_benchmark.py --preset full --solver deps/cadical/build/cadical --tag cadical_1c
python scripts/run_benchmark.py --preset full --solver deps/painless/painless --solver-cpus 32 --tag painless_32t

# 自訂參數
python scripts/run_benchmark.py --solver deps/cadical/build/cadical --timeout 300 --limit 20 --tag custom
```

### 分析結果

```bash
python scripts/score.py results/baseline/*.csv --timeout 1000 --output-dir results/baseline/
```

---

## 專案結構

```
SAT_Parallel/
├── README.md                     # 本文件
├── RESEARCH_PLAN.md              # 架構設計與決策
├── CMakeLists.txt                # 建置系統
├── env.sh                        # 環境變數（source 此檔）
├── requirements.txt              # Python 套件清單
├── config/
│   └── default_params.yaml       # 所有可調參數
├── docs/
│   └── IMPL_DETAILS.md           # 資料結構、公式、通訊協定
├── scripts/
│   ├── setup_env.sh              # 一鍵安裝腳本
│   ├── download_benchmarks.py    # 下載 SAT Competition benchmarks
│   ├── run_benchmark.py          # 執行 solver 並記錄結果
│   └── score.py                  # 計分與分析工具（PAR-2、cactus plot）
├── results/                      # benchmark 結果 CSV 與圖表
├── src/
│   ├── core/                     # DSRG 圖、中心性計算、聚合、GC
│   ├── master/                   # Master 邏輯、社群偵測、廣播
│   ├── worker/                   # Worker 邏輯、局部權重、Delta Patch
│   ├── gpu/                      # CUDA WalkSAT、GPU Prober
│   ├── comm/                     # lock-free queue、shared memory
│   └── solver/                   # Painless 框架 + CaDiCaL 介接
├── benchmarks/                   # SAT benchmark 實例
└── deps/                         # 本地建置的依賴（git ignored）
    ├── cadical/                  # CaDiCaL 3.0.0
    ├── painless/                 # Painless (lip6)
    ├── yaml-cpp/                 # yaml-cpp 0.8.0
    └── local/
        ├── openmpi/              # OpenMPI 5.0.6
        ├── lib/                  # 靜態連結庫
        └── include/              # 標頭檔
```

---

## 相依套件一覽

### C++ / CUDA

| 套件 | 版本 | 用途 |
|------|------|------|
| [CaDiCaL](https://github.com/arminbiere/cadical) | 3.0.0 | Worker 底層 CDCL 求解引擎 |
| [Painless](https://github.com/lip6/painless) | latest | 平行 SAT 框架（Master-Worker 架構） |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | YAML 設定檔解析 |
| [OpenMPI](https://www.open-mpi.org/) | 5.0.6 | Painless 所需的 MPI 通訊 |
| CUDA Toolkit | 11.5 / 12.4 | GPU Prober (WalkSAT) |

### Python（分析用）

| 套件 | 用途 |
|------|------|
| numpy | 數值計算 |
| pandas | 資料處理 |
| matplotlib | 繪圖 |
| scipy | 統計分析 |
| pyyaml | 讀取參數設定 |

---

## CUDA 注意事項

本專案設計目標為 NVIDIA RTX 4090（Ada Lovelace, Compute Capability 8.9）。

| CUDA 版本 | SM 架構 | 說明 |
|-----------|---------|------|
| 12.4（建議） | `sm_89` | 原生支援 RTX 4090，最佳效能 |
| 11.5（系統內建） | `sm_86` PTX | 透過 forward compatibility JIT 至 sm_89，首次啟動稍慢 |

環境變數 `SM_ARCH` 與 `CUDA_ARCH_FLAG` 會由 `env.sh` 自動設定。

---

## 設定參數

所有可調參數定義在 `config/default_params.yaml`，包括：

- **graph** — DSRG 邊建立/移除閾值、權重衰減率
- **centrality** — 中心性演算法選擇與參數
- **decision** — GraphScore 與 VSIDS 混合權重 (λ)
- **communication** — Delta Patch / Broadcast 間隔與大小
- **gc** — 圖清理頻率與淘汰條件
- **partitioning** — 社群偵測演算法與切割參數
- **gpu** — GPU Prober 回報間隔與 hotzone 大小

詳見 `docs/IMPL_DETAILS.md`。

---

## 參考文獻

| 文獻 | 用途 |
|------|------|
| Painless — Le Frioux et al., SAT 2017 | 平行框架 |
| CaDiCaL — Biere et al., SAT Competition 2020 | CDCL 引擎 |
| TurboSAT — Ozolins et al., 2022 | GPU 加速參考 |
| WalkSAT — Selman et al., AAAI 1994 | GPU 探測演算法 |
| Louvain — Blondel et al., JSTAT 2008 | 社群偵測 |
| Cube-and-Conquer — Heule et al., HVC 2012 | 圖切割參考 |
