#!/bin/sh
# code-handler.sh — Handle code tasks
set -eu

claude -p "You are an expert programmer. Complete this task: $(cat)"
