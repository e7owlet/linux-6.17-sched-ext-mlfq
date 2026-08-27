#!/usr/bin/env bash
set -euo pipefail

sudo apt update
sudo apt install -y \
	build-essential clang llvm gcc make pkg-config \
	libelf-dev libzstd-dev zlib1g-dev libcap-dev libbfd-dev libdw-dev \
	bpftool pahole trace-cmd linux-tools-common linux-tools-generic \
	stress-ng python3

printf 'Packages installed. Run scripts/check_env.sh next.\n'
