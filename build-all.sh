#!/usr/bin/env bash
set -e

echo "========================================================"
echo "Building pg_systemone Linux .so for PostgreSQL 14 - 17"
echo "========================================================"

docker compose up --build

echo ""
echo "========================================================"
echo "Build complete! Check the dist/ directory:"
echo "========================================================"
ls -lh dist/
