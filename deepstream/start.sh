#!/bin/bash
# Build the message library and MQTT adapter, then run the app.
set -e

make -C /deepstream-mount/msgconv
make -C /deepstream-mount/mqtt

exec /opt/nvidia/deepstream/deepstream/bin/deepstream-test5-app -c /deepstream-mount/config/app.txt
