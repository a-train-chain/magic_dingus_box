#!/bin/bash
#
# Magic Dingus Box - Web UI Test Runner
#
# Usage:
#   ./run_tests.sh                    # Test local server
#   ./run_tests.sh magicpi.local      # Test Pi over network
#   ./run_tests.sh 10.0.0.1           # Test Pi via USB
#   ./run_tests.sh --stress           # Run stress tests (large uploads)
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color
BOLD='\033[1m'

# Default configuration
HOST="${1:-localhost}"
PORT="${2:-5000}"
BASE_URL="http://${HOST}:${PORT}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_FILES_DIR="${SCRIPT_DIR}/test_files"
STRESS_MODE=false

# Parse arguments
for arg in "$@"; do
    case $arg in
        --stress)
            STRESS_MODE=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [host] [port] [--stress]"
            echo ""
            echo "Arguments:"
            echo "  host      Server hostname/IP (default: localhost)"
            echo "  port      Server port (default: 5000)"
            echo "  --stress  Run stress tests with large files"
            echo ""
            echo "Examples:"
            echo "  $0                        # Test localhost:5000"
            echo "  $0 magicpi.local          # Test Pi over WiFi"
            echo "  $0 10.0.0.1               # Test Pi over USB"
            echo "  $0 magicpi.local --stress # Stress test over WiFi"
            exit 0
            ;;
    esac
done

echo -e "${BOLD}==========================================${NC}"
echo -e "${BOLD}Magic Dingus Box Test Suite${NC}"
echo -e "${BOLD}==========================================${NC}"
echo -e "Target: ${BLUE}${BASE_URL}${NC}"
echo ""

# Check if server is reachable
echo -e "${YELLOW}Checking server connectivity...${NC}"
if ! curl -s --max-time 5 "${BASE_URL}/admin/health" > /dev/null; then
    echo -e "${RED}Error: Cannot connect to ${BASE_URL}${NC}"
    echo "Make sure the Flask server is running on the target."
    exit 1
fi
echo -e "${GREEN}Server is reachable!${NC}"
echo ""

# Create test files directory
mkdir -p "${TEST_FILES_DIR}"

# Function to create test video file
create_test_video() {
    local size_mb=$1
    local filename="${TEST_FILES_DIR}/test_video_${size_mb}mb.mp4"

    if [ -f "$filename" ]; then
        echo -e "  Using cached: ${filename}"
        return
    fi

    echo -e "  Creating ${size_mb}MB test video..."

    # Create a valid MP4 with ffmpeg if available, otherwise fake it
    if command -v ffmpeg &> /dev/null; then
        # Calculate duration based on size (roughly 1MB per second at low quality)
        local duration=$((size_mb))
        ffmpeg -f lavfi -i "color=c=blue:s=640x480:d=${duration}" \
               -f lavfi -i "sine=frequency=440:duration=${duration}" \
               -c:v libx264 -preset ultrafast -crf 51 \
               -c:a aac -b:a 32k \
               -y "${filename}" 2>/dev/null
    else
        # Create a fake MP4 header + padding
        echo -e "  (ffmpeg not found, creating dummy file)"
        printf '\x00\x00\x00\x1c\x66\x74\x79\x70\x69\x73\x6f\x6d\x00\x00\x00\x00\x69\x73\x6f\x6d\x61\x76\x63\x31\x6d\x70\x34\x31' > "${filename}"
        dd if=/dev/zero bs=1M count=$((size_mb - 1)) >> "${filename}" 2>/dev/null
    fi

    echo -e "${GREEN}  Created: ${filename}${NC}"
}

# Function to run upload test
test_upload() {
    local file=$1
    local desc=$2

    echo -e "\n${BOLD}Testing: ${desc}${NC}"

    if [ ! -f "$file" ]; then
        echo -e "${RED}  File not found: ${file}${NC}"
        return 1
    fi

    local size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file" 2>/dev/null)
    local size_mb=$((size / 1024 / 1024))
    echo -e "  File size: ${size_mb}MB"

    # Get CSRF token
    local csrf=$(curl -s "${BASE_URL}/admin/csrf-token" | python3 -c "import sys,json; print(json.load(sys.stdin).get('data',{}).get('token',''))" 2>/dev/null)

    # Upload with timing
    echo -e "  Uploading..."
    local start_time=$(date +%s.%N)

    local response=$(curl -s -X POST \
        -H "X-CSRF-Token: ${csrf}" \
        -F "file=@${file}" \
        -F "resolution=crt" \
        --max-time 900 \
        "${BASE_URL}/admin/smart-upload")

    local end_time=$(date +%s.%N)
    local duration=$(echo "$end_time - $start_time" | bc)
    local speed=$(echo "scale=2; $size_mb / $duration" | bc)

    # Check result
    local ok=$(echo "$response" | python3 -c "import sys,json; print(json.load(sys.stdin).get('ok', False))" 2>/dev/null)

    if [ "$ok" = "True" ]; then
        echo -e "${GREEN}  ✓ Upload successful${NC}"
        echo -e "  Duration: ${duration}s"
        echo -e "  Speed: ${speed} MB/s"
        return 0
    else
        echo -e "${RED}  ✗ Upload failed${NC}"
        echo -e "  Response: $response"
        return 1
    fi
}

# Function to check for transcoding completion
wait_for_transcode() {
    local job_id=$1
    local max_wait=${2:-300}  # Default 5 minutes

    echo -e "  Waiting for transcode (job: ${job_id})..."

    local elapsed=0
    while [ $elapsed -lt $max_wait ]; do
        local status=$(curl -s "${BASE_URL}/admin/transcode-status/${job_id}" | \
            python3 -c "import sys,json; d=json.load(sys.stdin).get('data',{}); print(d.get('status','unknown'), d.get('progress',0))" 2>/dev/null)

        local state=$(echo "$status" | cut -d' ' -f1)
        local progress=$(echo "$status" | cut -d' ' -f2)

        case $state in
            complete)
                echo -e "${GREEN}  ✓ Transcode complete${NC}"
                return 0
                ;;
            error)
                echo -e "${RED}  ✗ Transcode failed${NC}"
                return 1
                ;;
            *)
                printf "  Progress: %s%%\r" "$progress"
                sleep 2
                elapsed=$((elapsed + 2))
                ;;
        esac
    done

    echo -e "${RED}  ✗ Transcode timed out${NC}"
    return 1
}

# =========================================
# RUN API TESTS
# =========================================
echo -e "\n${BOLD}==========================================${NC}"
echo -e "${BOLD}Running API Tests${NC}"
echo -e "${BOLD}==========================================${NC}"

cd "${SCRIPT_DIR}"
python3 test_api.py "${BASE_URL}" -v

# =========================================
# RUN UPLOAD TESTS
# =========================================
echo -e "\n${BOLD}==========================================${NC}"
echo -e "${BOLD}Running Upload Tests${NC}"
echo -e "${BOLD}==========================================${NC}"

# Create test files
echo -e "\n${YELLOW}Preparing test files...${NC}"
create_test_video 5
create_test_video 50

# Run basic upload tests
test_upload "${TEST_FILES_DIR}/test_video_5mb.mp4" "Small video upload (5MB)"
test_upload "${TEST_FILES_DIR}/test_video_50mb.mp4" "Medium video upload (50MB)"

# =========================================
# STRESS TESTS (if requested)
# =========================================
if [ "$STRESS_MODE" = true ]; then
    echo -e "\n${BOLD}==========================================${NC}"
    echo -e "${BOLD}Running Stress Tests${NC}"
    echo -e "${BOLD}==========================================${NC}"

    echo -e "${YELLOW}Creating large test files...${NC}"
    create_test_video 200
    create_test_video 500

    test_upload "${TEST_FILES_DIR}/test_video_200mb.mp4" "Large video upload (200MB)"
    test_upload "${TEST_FILES_DIR}/test_video_500mb.mp4" "Very large video upload (500MB)"

    # Batch upload test
    echo -e "\n${BOLD}Testing: Batch upload (5 files)${NC}"
    for i in 1 2 3 4 5; do
        cp "${TEST_FILES_DIR}/test_video_5mb.mp4" "${TEST_FILES_DIR}/batch_test_${i}.mp4"
    done

    # Upload all batch files
    echo -e "  Uploading 5 files concurrently..."
    local csrf=$(curl -s "${BASE_URL}/admin/csrf-token" | python3 -c "import sys,json; print(json.load(sys.stdin).get('data',{}).get('token',''))" 2>/dev/null)

    local start_time=$(date +%s.%N)

    for i in 1 2 3 4 5; do
        curl -s -X POST \
            -H "X-CSRF-Token: ${csrf}" \
            -F "file=@${TEST_FILES_DIR}/batch_test_${i}.mp4" \
            -F "resolution=crt" \
            "${BASE_URL}/admin/smart-upload" > /dev/null &
    done
    wait

    local end_time=$(date +%s.%N)
    local duration=$(echo "$end_time - $start_time" | bc)
    echo -e "${GREEN}  ✓ Batch upload completed in ${duration}s${NC}"

    # Cleanup batch files
    rm -f "${TEST_FILES_DIR}"/batch_test_*.mp4
fi

# =========================================
# SUMMARY
# =========================================
echo -e "\n${BOLD}==========================================${NC}"
echo -e "${BOLD}Test Summary${NC}"
echo -e "${BOLD}==========================================${NC}"
echo -e "${GREEN}All tests completed!${NC}"
echo ""
echo "Test files are cached in: ${TEST_FILES_DIR}"
echo "To clean up: rm -rf ${TEST_FILES_DIR}"

# =========================================
# CONNECTION TYPE DETECTION
# =========================================
echo -e "\n${BOLD}Connection Info:${NC}"
if [[ "$HOST" == "10.0.0.1" ]]; then
    echo -e "  Connection: ${GREEN}USB Ethernet Gadget${NC} (fast)"
elif [[ "$HOST" == "localhost" || "$HOST" == "127.0.0.1" ]]; then
    echo -e "  Connection: ${GREEN}Local${NC}"
else
    echo -e "  Connection: ${YELLOW}WiFi${NC} (may be slower for large files)"
    echo -e "  Tip: Use USB connection for faster uploads:"
    echo -e "       ./run_tests.sh 10.0.0.1"
fi
