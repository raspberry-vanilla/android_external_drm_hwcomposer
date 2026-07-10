#!/usr/bin/env bash

function safe_repo_sync() {
  local jobs="${1:-4}"
  local max_retries=5
  local retry_delay=30
  local count=0

  until repo sync --fail-fast --no-tags --current-branch --no-clone-bundle -j"${jobs}"; do
    count=$((count + 1))
    if [ $count -ge $max_retries ]; then
      echo "ERROR: Repo sync failed after $max_retries attempts."
      return 1
    fi
    echo "WARNING: Repo sync failed. Retrying in $retry_delay seconds (attempt $count/$max_retries)..."
    sleep $retry_delay
    retry_delay=$((retry_delay * 2)) # Exponential backoff
  done
}
