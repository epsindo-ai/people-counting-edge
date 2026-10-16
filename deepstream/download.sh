#!/bin/bash
# Download PeopleNet from NGC (needs the ngc CLI). Skips if already there.
set -e

MODEL_DIR=/deepstream-mount/models
if [ -f "$MODEL_DIR/peoplenet_vdeployable_quantized_onnx_v2.6.3/resnet34_peoplenet.onnx" ]; then
  echo "PeopleNet already downloaded"
  exit 0
fi

ngc registry model download-version \
  "nvidia/tao/peoplenet:deployable_quantized_onnx_v2.6.3" \
  --dest "$MODEL_DIR/"
