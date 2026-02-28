#!/bin/sh
# docs-handler.sh — Handle documentation tasks
set -eu

claude -p "You are a technical writer. Complete this task: $(cat)"
