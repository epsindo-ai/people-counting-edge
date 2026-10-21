#!/bin/bash
# Build the message library and MQTT adapter, then run the app.
set -e

make -C /deepstream-mount/msgconv
make -C /deepstream-mount/mqtt

exec python3 /deepstream-mount/supervise.py
