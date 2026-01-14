#!/bin/bash
# Run tests under valgrind in Docker to check for memory leaks
#
# Usage:
#   ./docker-valgrind.sh              # Run all tests
#   ./docker-valgrind.sh test_name    # Run specific test (e.g., test_gstd_session)
#
# Output shows:
#   - definitely lost: Real memory leaks (should be 0)
#   - indirectly lost: Memory reachable only through leaked blocks
#   - possibly lost: Memory that might be leaked
#   - still reachable: Memory not freed but still accessible (often OK)

set -e

IMAGE_NAME="gstd-valgrind"
TEST_NAME="${1:-}"

echo "Building Docker image with valgrind..."
docker build -f Dockerfile.valgrind -t "$IMAGE_NAME" .

echo ""
if [ -n "$TEST_NAME" ]; then
    echo "Running valgrind on: $TEST_NAME"
    docker run --rm "$IMAGE_NAME" sh -c "
        cd /gstd/build && \
        valgrind --leak-check=full \
                 --show-leak-kinds=definite,indirect \
                 --errors-for-leak-kinds=definite \
                 --track-origins=yes \
                 --suppressions=/gstd/tests/gstd.supp \
                 --error-exitcode=1 \
                 ./tests/gstd/$TEST_NAME 2>&1 | tee /tmp/valgrind.log; \
        echo ''; \
        echo '=== LEAK SUMMARY ==='; \
        grep -A5 'LEAK SUMMARY' /tmp/valgrind.log || echo 'No leaks detected'
    "
else
    echo "Running valgrind on all gstd tests..."
    docker run --rm "$IMAGE_NAME" sh -c '
        cd /gstd/build
        FAILED=0
        for test in tests/gstd/test_gstd_*; do
            if [ -x "$test" ]; then
                TEST_NAME=$(basename "$test")
                echo ""
                echo "========================================"
                echo "Testing: $TEST_NAME"
                echo "========================================"

                if valgrind --leak-check=full \
                            --show-leak-kinds=definite,indirect \
                            --errors-for-leak-kinds=definite \
                            --track-origins=yes \
                            --suppressions=/gstd/tests/gstd.supp \
                            --error-exitcode=1 \
                            "$test" 2>&1 | tee /tmp/valgrind.log; then
                    echo "✓ $TEST_NAME: PASS (no definite leaks)"
                else
                    echo "✗ $TEST_NAME: FAIL"
                    grep -A5 "LEAK SUMMARY" /tmp/valgrind.log || true
                    FAILED=$((FAILED + 1))
                fi
            fi
        done

        echo ""
        echo "========================================"
        echo "VALGRIND SUMMARY"
        echo "========================================"
        if [ $FAILED -eq 0 ]; then
            echo "All tests passed with no definite memory leaks!"
            exit 0
        else
            echo "$FAILED test(s) had memory leaks"
            exit 1
        fi
    '
fi
