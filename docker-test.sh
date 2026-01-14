#!/bin/bash
# Simple script to build and run tests in Docker

set -e

IMAGE_NAME="gstd-test"

echo "Building Docker image..."
docker build -t "$IMAGE_NAME" .

echo ""
echo "Running tests..."
docker run --rm "$IMAGE_NAME"
