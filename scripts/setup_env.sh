#!/usr/bin/env bash
# ==============================================================================
# SAT_Parallel — Environment Setup Script
# ==============================================================================
#
# Sets up the complete build environment for SAT_Parallel:
#   1. Validate system prerequisites (gcc, cmake, git, GPU, CUDA >= 12.x)
#   2. Detect system CUDA toolkit (requires CUDA >= 12.x for native sm_89)
#   3. Install OpenMPI locally (required by Painless)
#   4. Build CaDiCaL solver from source
#   5. Build Painless parallel framework (with all bundled solvers)
#   6. Build yaml-cpp library from source
#   7. Create Python virtual environment with analysis packages
#   8. Generate env.sh for future sessions
#
# Prerequisites: CUDA toolkit >= 12.x must be installed system-wide.
#
# Usage:
#   bash scripts/setup_env.sh
# ==============================================================================

set -euo pipefail

# --------------- colour helpers ---------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[ OK ]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
fail()  { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }
section() { echo -e "\n${CYAN}========== $* ==========${NC}\n"; }

# --------------- paths ---------------
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="$PROJECT_ROOT/deps"
LOCAL_PREFIX="$DEPS_DIR/local"
OPENMPI_PREFIX="$LOCAL_PREFIX/openmpi"
VENV_DIR="$PROJECT_ROOT/.venv"
ENV_FILE="$PROJECT_ROOT/env.sh"
NPROC=$(nproc 2>/dev/null || echo 4)
JOBS=$(( NPROC > 8 ? 8 : NPROC ))

mkdir -p "$DEPS_DIR" "$LOCAL_PREFIX"/{lib,include,bin}

# ==============================================================================
section "1/8  System Prerequisites"
# ==============================================================================

check_cmd() {
    if command -v "$1" &>/dev/null; then
        ok "$1  ->  $(command -v "$1")"
    else
        fail "$1 not found — please install it first"
    fi
}

check_cmd gcc
check_cmd g++
check_cmd cmake
check_cmd make
check_cmd git
check_cmd wget

GCC_VER=$(gcc -dumpversion)
CMAKE_VER=$(cmake --version | head -1 | awk '{print $3}')
info "GCC $GCC_VER | CMake $CMAKE_VER | $NPROC CPUs | jobs=$JOBS"

GPU_CC="0.0"
if nvidia-smi &>/dev/null; then
    GPU_NAME=$(nvidia-smi --query-gpu=gpu_name --format=csv,noheader | head -1)
    GPU_CC=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -1)
    DRIVER_VER=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -1)
    ok "GPU: $GPU_NAME (CC $GPU_CC) | Driver $DRIVER_VER"
else
    warn "nvidia-smi not found — GPU features will be disabled"
fi

# ==============================================================================
section "2/8  CUDA Toolkit (detect system install)"
# ==============================================================================

CUDA_SEARCH_PATHS=(
    "/usr/local/cuda/bin/nvcc"
    "/usr/local/cuda-12.8/bin/nvcc"
    "/usr/local/cuda-12.6/bin/nvcc"
    "/usr/local/cuda-12.4/bin/nvcc"
)

CUDA_HOME=""
for p in "${CUDA_SEARCH_PATHS[@]}"; do
    if [ -x "$p" ]; then
        CUDA_HOME="$(dirname "$(dirname "$p")")"
        break
    fi
done

if [ -z "$CUDA_HOME" ]; then
    fail "No CUDA toolkit found. Install CUDA 12.x (>= 12.4) with sm_89 support for RTX 4090."
fi

NVCC_BIN="${CUDA_HOME}/bin/nvcc"
NVCC_VER=$("$NVCC_BIN" --version 2>/dev/null | grep "release" | sed 's/.*release //' | sed 's/,.*//')
NVCC_MAJOR=$(echo "$NVCC_VER" | cut -d. -f1)

if [ "$NVCC_MAJOR" -lt 12 ]; then
    fail "CUDA $NVCC_VER is too old. Need CUDA >= 12.x for native sm_89 (RTX 4090)."
fi

ok "nvcc $NVCC_VER at $NVCC_BIN"

CUDA_ARCH_FLAG="-gencode arch=compute_89,code=sm_89"
SM_ARCH="89"

# ==============================================================================
section "3/8  OpenMPI (local install)"
# ==============================================================================

OPENMPI_VER="5.0.6"
OPENMPI_URL="https://download.open-mpi.org/release/open-mpi/v5.0/openmpi-${OPENMPI_VER}.tar.gz"

if [ -x "$OPENMPI_PREFIX/bin/mpic++" ]; then
    ok "OpenMPI already installed at $OPENMPI_PREFIX"
else
    OPENMPI_SRC="$DEPS_DIR/openmpi-${OPENMPI_VER}"
    OPENMPI_TAR="$DEPS_DIR/openmpi-${OPENMPI_VER}.tar.gz"

    if [ ! -f "$OPENMPI_TAR" ]; then
        info "Downloading OpenMPI $OPENMPI_VER ..."
        wget -q --show-progress -O "$OPENMPI_TAR" "$OPENMPI_URL"
    fi

    if [ ! -d "$OPENMPI_SRC" ]; then
        info "Extracting OpenMPI ..."
        tar -xzf "$OPENMPI_TAR" -C "$DEPS_DIR"
    fi

    info "Configuring OpenMPI (this may take a few minutes) ..."
    cd "$OPENMPI_SRC"
    ./configure --prefix="$OPENMPI_PREFIX" \
                --disable-mpi-fortran \
                --enable-shared \
                --enable-static \
                --without-verbs \
                --without-ucx \
                --without-ofi \
                --without-psm2 \
                --quiet 2>&1 | tail -5

    info "Building OpenMPI (using $JOBS cores) ..."
    make -j"$JOBS" 2>&1 | tail -3

    info "Installing OpenMPI ..."
    make install 2>&1 | tail -3
    cd "$PROJECT_ROOT"

    if [ -x "$OPENMPI_PREFIX/bin/mpic++" ]; then
        ok "OpenMPI $OPENMPI_VER installed at $OPENMPI_PREFIX"
    else
        fail "OpenMPI build failed"
    fi
fi

export PATH="$OPENMPI_PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$OPENMPI_PREFIX/lib:${LD_LIBRARY_PATH:-}"

# ==============================================================================
section "4/8  CaDiCaL Solver"
# ==============================================================================

CADICAL_DIR="$DEPS_DIR/cadical"
CADICAL_LIB="$CADICAL_DIR/build/libcadical.a"

if [ -f "$CADICAL_LIB" ]; then
    ok "CaDiCaL already built at $CADICAL_LIB"
else
    if [ ! -d "$CADICAL_DIR" ]; then
        info "Cloning CaDiCaL ..."
        git clone --depth 1 https://github.com/arminbiere/cadical.git "$CADICAL_DIR"
    fi

    info "Building CaDiCaL ..."
    cd "$CADICAL_DIR"
    ./configure
    make -j"$JOBS"
    cd "$PROJECT_ROOT"

    [ -f "$CADICAL_LIB" ] && ok "CaDiCaL built successfully" || fail "CaDiCaL build failed"
fi

# ==============================================================================
section "5/8  Painless Framework"
# ==============================================================================

PAINLESS_DIR="$DEPS_DIR/painless"

if [ -f "$PAINLESS_DIR/painless" ] || [ -f "$PAINLESS_DIR/painless_release" ]; then
    ok "Painless already built"
else
    if [ ! -d "$PAINLESS_DIR" ] || [ -f "$PAINLESS_DIR/.placeholder" ]; then
        rm -rf "$PAINLESS_DIR"
        info "Cloning Painless (lip6/painless) ..."
        git clone --depth 1 https://github.com/lip6/painless.git "$PAINLESS_DIR" || {
            warn "Could not clone Painless — creating placeholder"
            mkdir -p "$PAINLESS_DIR"
            touch "$PAINLESS_DIR/.placeholder"
        }
    fi

    if [ -f "$PAINLESS_DIR/Makefile" ] && [ ! -f "$PAINLESS_DIR/.placeholder" ]; then
        info "Building Painless bundled solvers ..."
        cd "$PAINLESS_DIR"

        make solvers -j"$JOBS" 2>&1 | tail -10
        ok "Solvers built"

        info "Building Painless m4ri library ..."
        make libs -j"$JOBS" 2>&1 | tail -5
        ok "m4ri built"

        info "Building Painless main binary ..."
        make -j"$JOBS" 2>&1 | tail -10

        cd "$PROJECT_ROOT"

        if [ -f "$PAINLESS_DIR/painless" ] || [ -f "$PAINLESS_DIR/painless_release" ]; then
            ok "Painless built successfully"
        else
            warn "Painless build had issues — check logs above"
        fi
    else
        warn "Painless source not available — skipping build"
    fi
fi

# ==============================================================================
section "6/8  yaml-cpp Library"
# ==============================================================================

YAMLCPP_DIR="$DEPS_DIR/yaml-cpp"
YAMLCPP_LIB="$LOCAL_PREFIX/lib/libyaml-cpp.a"

if [ -f "$YAMLCPP_LIB" ]; then
    ok "yaml-cpp already installed at $YAMLCPP_LIB"
else
    if [ ! -d "$YAMLCPP_DIR" ]; then
        info "Cloning yaml-cpp ..."
        git clone --depth 1 --branch 0.8.0 https://github.com/jbeder/yaml-cpp.git "$YAMLCPP_DIR"
    fi

    info "Building yaml-cpp ..."
    mkdir -p "$YAMLCPP_DIR/build"
    cd "$YAMLCPP_DIR/build"
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$LOCAL_PREFIX" \
        -DYAML_CPP_BUILD_TESTS=OFF \
        -DYAML_CPP_BUILD_TOOLS=OFF \
        -DYAML_BUILD_SHARED_LIBS=OFF
    make -j"$JOBS"
    make install
    cd "$PROJECT_ROOT"

    [ -f "$YAMLCPP_LIB" ] && ok "yaml-cpp installed" || fail "yaml-cpp build failed"
fi

# ==============================================================================
section "7/8  Python Virtual Environment"
# ==============================================================================

if [ -f "$VENV_DIR/bin/python" ]; then
    ok "Python venv already exists at $VENV_DIR"
else
    info "Creating Python virtual environment ..."
    python3 -m venv "$VENV_DIR" 2>/dev/null || {
        info "python3-venv not available, trying --without-pip ..."
        python3 -m venv --without-pip "$VENV_DIR"
    }
    ok "venv created"
fi

info "Ensuring pip is available ..."
if ! "$VENV_DIR/bin/python" -m pip --version &>/dev/null; then
    info "Bootstrapping pip via get-pip.py ..."
    wget -q -O "$DEPS_DIR/get-pip.py" https://bootstrap.pypa.io/get-pip.py
    "$VENV_DIR/bin/python" "$DEPS_DIR/get-pip.py"
fi

info "Upgrading pip ..."
"$VENV_DIR/bin/python" -m pip install --upgrade pip -q

info "Installing Python packages ..."
"$VENV_DIR/bin/python" -m pip install -r "$PROJECT_ROOT/requirements.txt" -q

ok "Python packages installed"
"$VENV_DIR/bin/python" -m pip list --format=columns 2>/dev/null | grep -iE "numpy|pandas|matplotlib|scipy|PyYAML" || true

# ==============================================================================
section "8/8  Generate env.sh & Verify"
# ==============================================================================

CUDA_HOME_FINAL="${CUDA_HOME:-/usr}"

cat > "$ENV_FILE" << ENVEOF
#!/usr/bin/env bash
# Auto-generated by setup_env.sh — source this before building/running
# Usage:  source env.sh

export PROJECT_ROOT="$PROJECT_ROOT"
export DEPS_DIR="$DEPS_DIR"
export LOCAL_PREFIX="$LOCAL_PREFIX"

# CUDA
export CUDA_HOME="$CUDA_HOME_FINAL"
export PATH="\$CUDA_HOME/bin:\$PATH"
export LD_LIBRARY_PATH="\$CUDA_HOME/lib64:\${LD_LIBRARY_PATH:-}"
export CUDA_ARCH_FLAG="$CUDA_ARCH_FLAG"
export SM_ARCH="$SM_ARCH"

# OpenMPI
export PATH="$OPENMPI_PREFIX/bin:\$PATH"
export LD_LIBRARY_PATH="$OPENMPI_PREFIX/lib:\$LD_LIBRARY_PATH"

# Local libraries (yaml-cpp, etc.)
export CMAKE_PREFIX_PATH="$LOCAL_PREFIX:\${CMAKE_PREFIX_PATH:-}"
export LD_LIBRARY_PATH="$LOCAL_PREFIX/lib:\$LD_LIBRARY_PATH"
export CPATH="$LOCAL_PREFIX/include:\${CPATH:-}"

# CaDiCaL
export CADICAL_DIR="$CADICAL_DIR"
export CADICAL_LIB="$CADICAL_DIR/build/libcadical.a"
export CADICAL_INCLUDE="$CADICAL_DIR/src"

# Painless
export PAINLESS_DIR="$DEPS_DIR/painless"

# Python venv
export VIRTUAL_ENV="$VENV_DIR"
export PATH="\$VIRTUAL_ENV/bin:\$PATH"
ENVEOF

chmod +x "$ENV_FILE"
ok "env.sh written to $ENV_FILE"

# --------------- verification ---------------
echo ""
section "Verification Summary"

verify() {
    local label="$1" check="$2"
    if eval "$check" &>/dev/null; then
        ok "$label"
    else
        warn "$label — MISSING"
    fi
}

verify "GCC/G++             " "g++ --version"
verify "CMake                " "cmake --version"
verify "CUDA nvcc            " "test -x $CUDA_HOME_FINAL/bin/nvcc"
verify "OpenMPI mpic++       " "test -x $OPENMPI_PREFIX/bin/mpic++"
verify "CaDiCaL (libcadical) " "test -f $CADICAL_DIR/build/libcadical.a"
verify "Painless             " "test -f $DEPS_DIR/painless/painless -o -f $DEPS_DIR/painless/painless_release"
verify "yaml-cpp             " "test -f $LOCAL_PREFIX/lib/libyaml-cpp.a"
verify "Python venv          " "test -f $VENV_DIR/bin/python"
verify "numpy                " "$VENV_DIR/bin/python -c 'import numpy'"
verify "pandas               " "$VENV_DIR/bin/python -c 'import pandas'"
verify "matplotlib           " "$VENV_DIR/bin/python -c 'import matplotlib'"
verify "pyyaml               " "$VENV_DIR/bin/python -c 'import yaml'"

echo ""
info "Project source directories:"
find "$PROJECT_ROOT/src" -type d | sort | sed "s|$PROJECT_ROOT/||"
echo ""
ok "Setup complete!"
echo ""
info "Next steps:"
echo "  1.  source env.sh"
echo "  2.  mkdir -p build && cd build"
echo "  3.  cmake .. -DCMAKE_BUILD_TYPE=Release"
echo "  4.  make -j${JOBS}"
echo ""
