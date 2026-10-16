# Builds NVIDIA's MQTT adapter (DeepStream 7.1) with 2 fixes (reconnect-fixes.patch).
# Stock adapter, when the broker goes down:
#   - a publish can deadlock nvmsgbroker -> pipeline freezes
#   - every failed reconnect leaks a mosquitto client (extra connections later)
# Run: make

DS_DIR ?= /opt/nvidia/deepstream/deepstream
SRC_DIR := $(DS_DIR)/sources/libs/mqtt_protocol_adaptor

LIB := libnvds_mqtt_proto_fixed.so

CXXFLAGS := -O2 -Wall -fPIC \
	-I$(DS_DIR)/sources/includes -I/usr/local/include \
	$(shell pkg-config --cflags glib-2.0)

LIBS := -L$(DS_DIR)/lib -lnvds_logger -Wl,-rpath,$(DS_DIR)/lib \
	-L/usr/local/lib -lmosquitto -lpthread -lcrypto \
	$(shell pkg-config --libs glib-2.0)

all: $(LIB)

build/nvds_mqtt_proto.cpp: $(SRC_DIR)/nvds_mqtt_proto.cpp reconnect-fixes.patch
	mkdir -p build
	cp $< $@
	patch --forward --no-backup-if-mismatch $@ < reconnect-fixes.patch

$(LIB): build/nvds_mqtt_proto.cpp Makefile
	$(CXX) -shared -o $@ $(CXXFLAGS) $< $(DS_DIR)/sources/libs/nvds_msgapi_common_src/nvds_utils.cpp $(LIBS)

clean:
	rm -rf build $(LIB)

.PHONY: all clean
