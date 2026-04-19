#!/usr/bin/env bash
# =============================================================================
# vcenter_test.sh — Full MiniFS + RocksDB functional test on a Linux VM.
#
# Usage:
#   chmod +x vcenter_test.sh
#   ./vcenter_test.sh [OPTIONS]
#
# Options:
#   --src    <path>   Path to MiniFS source (default: auto-detect)
#   --img    <path>   Path for test image file (default: /tmp/nebula_test.img)
#   --size   <size>   Image size, e.g. 1G 2G 512M (default: 2G)
#   --count  <N>      Number of RocksDB key-value pairs (default: 1000)
#   --vsize  <B>      Value size in bytes (default: 512)
#   --jobs   <N>      Parallel build jobs (default: nproc)
#   --keep           Keep image file after test (default: delete)
#   --help
#
# What it does:
#   1. Install dependencies (librocksdb-dev, cmake, build-essential)
#   2. Build MiniFS with -DNEBULA_ENABLE_ROCKSDB=ON
#   3. Format a fresh Nebula image
#   4. Run smoke test  (10 keys)
#   5. Run stress test (--count keys, --vsize bytes each)
#   6. Run re-open test (verifies RocksDB WAL replay)
#   7. Print summary
# =============================================================================
set -euo pipefail

# ---------- Colours ----------
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; NC='\033[0m'
pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAILURES=$((FAILURES+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

FAILURES=0

# ---------- Defaults ----------
SRC_DIR=""
IMG_FILE="/tmp/nebula_test.img"
IMG_SIZE="2G"
COUNT=1000
VSIZE=512
JOBS=$(nproc 2>/dev/null || echo 4)
KEEP_IMG=0

# ---------- Argument parsing ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --src)    SRC_DIR="$2";  shift 2 ;;
        --img)    IMG_FILE="$2"; shift 2 ;;
        --size)   IMG_SIZE="$2"; shift 2 ;;
        --count)  COUNT="$2";    shift 2 ;;
        --vsize)  VSIZE="$2";    shift 2 ;;
        --jobs)   JOBS="$2";     shift 2 ;;
        --keep)   KEEP_IMG=1;    shift   ;;
        --help|-h)
            grep '^#' "$0" | head -20 | sed 's/^# \{0,2\}//'
            exit 0 ;;
        *) echo "Unknown arg: $1"; exit 2 ;;
    esac
done

# ---------- Locate source ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "$SRC_DIR" ]]; then
    # Script lives in MiniFS/scripts/ — parent is source root
    SRC_DIR="$(dirname "$SCRIPT_DIR")"
fi

if [[ ! -f "$SRC_DIR/CMakeLists.txt" ]]; then
    echo "Cannot find MiniFS source at $SRC_DIR"
    echo "Pass --src /path/to/MiniFS"
    exit 1
fi

BUILD_DIR="$SRC_DIR/build_vcenter"

echo ""
echo "=================================================="
echo "  MiniFS + RocksDB vCenter Functional Test"
echo "=================================================="
info "Source:    $SRC_DIR"
info "Build:     $BUILD_DIR"
info "Image:     $IMG_FILE ($IMG_SIZE)"
info "Keys:      $COUNT  |  Value size: ${VSIZE}B"
echo ""

# =============================================================================
# Step 1 — Install dependencies
# =============================================================================
info "Step 1: Checking / installing dependencies..."

MISSING=()
dpkg -l librocksdb-dev &>/dev/null || MISSING+=(librocksdb-dev)
dpkg -l cmake         &>/dev/null || MISSING+=(cmake)
dpkg -l build-essential &>/dev/null || MISSING+=(build-essential)

if [[ ${#MISSING[@]} -gt 0 ]]; then
    info "Installing: ${MISSING[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y -qq "${MISSING[@]}"
fi

# Verify RocksDB version >= 7
RDB_VER=$(dpkg -l librocksdb-dev 2>/dev/null | grep rocksdb | awk '{print $3}' | cut -d. -f1)
if [[ -z "$RDB_VER" ]] || [[ "$RDB_VER" -lt 7 ]]; then
    fail "librocksdb-dev version $RDB_VER is too old (need >= 7.0)"
    exit 1
fi
pass "Dependencies OK  (librocksdb-dev $(dpkg -l librocksdb-dev | grep rocksdb | awk '{print $3}'))"

# =============================================================================
# Step 2 — Build
# =============================================================================
info "Step 2: Building MiniFS (jobs=$JOBS)..."
mkdir -p "$BUILD_DIR"

cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DNEBULA_ENABLE_ROCKSDB=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
    -Wno-dev \
    > "$BUILD_DIR/cmake_configure.log" 2>&1

cmake --build "$BUILD_DIR" -j"$JOBS" \
    > "$BUILD_DIR/cmake_build.log" 2>&1

for tool in nebula_format nebula_rocksdb_test; do
    if [[ ! -x "$BUILD_DIR/$tool" ]]; then
        fail "Build failed — $tool not found. See $BUILD_DIR/cmake_build.log"
        exit 1
    fi
done
pass "Build OK"

FORMAT="$BUILD_DIR/nebula_format"
RDB_TEST="$BUILD_DIR/nebula_rocksdb_test"

# =============================================================================
# Step 3 — Format image
# =============================================================================
info "Step 3: Formatting Nebula image ($IMG_SIZE)..."
rm -f "$IMG_FILE"

if ! "$FORMAT" --path "$IMG_FILE" --size "$IMG_SIZE" --force 2>/dev/null; then
    fail "nebula_format failed"
    exit 1
fi
pass "Format OK  ($(du -sh "$IMG_FILE" | cut -f1) on disk)"

# =============================================================================
# Step 4 — Smoke test (10 keys)
# =============================================================================
info "Step 4: Smoke test (10 keys, 8B values)..."
if "$RDB_TEST" --count 10 --value-size 8 "$IMG_FILE" 2>/dev/null \
        | grep -q '\[PASS\]'; then
    pass "Smoke test OK"
else
    fail "Smoke test FAILED"
fi

# =============================================================================
# Step 5 — Stress test (N keys, V-byte values)
# =============================================================================
info "Step 5: Stress test ($COUNT keys, ${VSIZE}B values)..."

# Format a fresh image for stress test
rm -f "$IMG_FILE"
"$FORMAT" --path "$IMG_FILE" --size "$IMG_SIZE" --force 2>/dev/null

START=$(date +%s%3N)
if "$RDB_TEST" --count "$COUNT" --value-size "$VSIZE" "$IMG_FILE" 2>/dev/null \
        | grep -q '\[PASS\]'; then
    END=$(date +%s%3N)
    ELAPSED=$(( END - START ))
    THROUGHPUT=$(( COUNT * 1000 / (ELAPSED + 1) ))
    pass "Stress test OK  ($COUNT keys in ${ELAPSED}ms ≈ ${THROUGHPUT} ops/sec)"
else
    fail "Stress test FAILED"
fi

# =============================================================================
# Step 6 — Re-open test (WAL replay)
# =============================================================================
info "Step 6: Re-open test (RocksDB WAL replay)..."

# Format fresh, write keys, then re-open and read — verifies durability
rm -f "$IMG_FILE"
"$FORMAT" --path "$IMG_FILE" --size "$IMG_SIZE" --force 2>/dev/null

# First open: write 100 keys
if ! "$RDB_TEST" --count 100 --value-size 64 "$IMG_FILE" 2>/dev/null \
        | grep -q '\[PASS\]'; then
    fail "Re-open test: initial write failed"
else
    # Second open on SAME image (no reformat): read should still pass
    if "$RDB_TEST" --count 100 --value-size 64 "$IMG_FILE" 2>/dev/null \
            | grep -q '\[PASS\]'; then
        pass "Re-open test OK  (WAL replay successful)"
    else
        fail "Re-open test: second open failed (WAL replay broken)"
    fi
fi

# =============================================================================
# Step 7 — Large value test (1MB values)
# =============================================================================
info "Step 7: Large value test (50 keys × 1MB)..."
rm -f "$IMG_FILE"
"$FORMAT" --path "$IMG_FILE" --size "$IMG_SIZE" --force 2>/dev/null

if "$RDB_TEST" --count 50 --value-size 1048576 "$IMG_FILE" 2>/dev/null \
        | grep -q '\[PASS\]'; then
    pass "Large value test OK"
else
    fail "Large value test FAILED"
fi

# =============================================================================
# Cleanup
# =============================================================================
if [[ $KEEP_IMG -eq 0 ]]; then
    rm -f "$IMG_FILE"
    info "Image file removed (pass --keep to retain)"
fi

# =============================================================================
# Summary
# =============================================================================
echo ""
echo "=================================================="
if [[ $FAILURES -eq 0 ]]; then
    echo -e "${GREEN}  ALL TESTS PASSED${NC}"
else
    echo -e "${RED}  $FAILURES TEST(S) FAILED${NC}"
fi
echo "=================================================="
echo ""

exit $FAILURES
