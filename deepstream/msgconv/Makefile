# Builds the people counting message library used by nvmsgconv.
# Works on x86 (dGPU) and Jetson. Run: make

DS_DIR ?= /opt/nvidia/deepstream/deepstream

LIB := libnvds_msgconv_peoplecount.so
SRC := peoplecount_msgconv.cpp

CXXFLAGS := -O2 -Wall -fPIC -std=c++17 \
	-I$(DS_DIR)/sources/includes \
	-I$(DS_DIR)/sources/libs/nvmsgconv \
	$(shell pkg-config --cflags gstreamer-1.0 json-glib-1.0)

LIBS := -L$(DS_DIR)/lib -lnvds_meta -Wl,-rpath,$(DS_DIR)/lib \
	$(shell pkg-config --libs gstreamer-1.0 json-glib-1.0)

all: $(LIB)

$(LIB): $(SRC) Makefile
	$(CXX) -shared -o $@ $(CXXFLAGS) $< $(LIBS)

clean:
	rm -f $(LIB)

.PHONY: all clean
