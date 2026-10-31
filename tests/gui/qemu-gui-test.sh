#!/bin/bash
exec python3 "$(dirname "$0")/qemu_gui_test.py" "$@"
