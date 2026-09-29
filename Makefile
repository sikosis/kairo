CXX ?= c++
AR ?= ar
PKG_CONFIG ?= pkg-config
CURL_CONFIG ?= curl-config
KAIRO_USE_CURL ?= 1
UNAME_S := $(shell uname -s)

CPPFLAGS := -Icore/include -Icore/src
ifeq ($(UNAME_S),Haiku)
CPPFLAGS += -Ivendor/haiku-x86_64/include
endif
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDFLAGS += -pthread

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null || $(CURL_CONFIG) --cflags 2>/dev/null)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || $(CURL_CONFIG) --libs 2>/dev/null)
ifeq ($(KAIRO_USE_CURL),1)
CPPFLAGS += -DKAIRO_HAS_CURL=1 $(CURL_CFLAGS)
PROVIDER_LIBS := $(if $(strip $(CURL_LIBS)),$(CURL_LIBS),-lcurl)
else
CPPFLAGS += -DKAIRO_HAS_CURL=0
endif

CORE_SOURCES := \
	core/src/agent_engine.cpp \
	core/src/events.cpp \
	core/src/json.cpp \
	core/src/model.cpp \
	core/src/openai_provider.cpp \
	core/src/session_store.cpp \
	core/src/sse_decoder.cpp \
	core/src/workspace.cpp
CORE_OBJECTS := $(CORE_SOURCES:%.cpp=build/%.o)

GUI_PROVIDER_LIBS := $(PROVIDER_LIBS)
ifeq ($(UNAME_S),Haiku)
ifeq ($(KAIRO_USE_CURL),1)
BUNDLED_CURL_DIR := vendor/haiku-x86_64/lib
BUNDLED_CURL_OUTPUTS := build/libcurl.so build/libcurl.so.4 build/libcurl.so.4.8.0
GUI_PROVIDER_LIBS := -L$(BUNDLED_CURL_DIR) -lcurl
GUI_RPATH := -Wl,-rpath,'$$ORIGIN'
endif
endif

.PHONY: all clean test check gui
all: build/kairo-cli $(if $(filter Haiku,$(UNAME_S)),build/kairo-gui)

build/libkairo.a: $(CORE_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

build/kairo-cli: build/apps/cli/main.o build/libkairo.a
	$(CXX) $(LDFLAGS) -o $@ $^ $(PROVIDER_LIBS)

build/kairo-tests: build/tests/test_main.o build/libkairo.a
	$(CXX) $(LDFLAGS) -o $@ $^ $(PROVIDER_LIBS)

build/kairo-gui: build/apps/gui/main.o build/libkairo.a $(BUNDLED_CURL_OUTPUTS)
	$(CXX) $(LDFLAGS) $(GUI_RPATH) -o $@ build/apps/gui/main.o build/libkairo.a $(GUI_PROVIDER_LIBS) -lbe -ltracker

build/libcurl.so.4.8.0: vendor/haiku-x86_64/lib/libcurl.so.4.8.0
	@mkdir -p $(@D)
	cp $< $@

build/libcurl.so.4: vendor/haiku-x86_64/lib/libcurl.so.4
	@mkdir -p $(@D)
	cp -P $< $@

build/libcurl.so: vendor/haiku-x86_64/lib/libcurl.so
	@mkdir -p $(@D)
	cp -P $< $@

gui: build/kairo-gui

test: build/kairo-tests
	./build/kairo-tests

check: all test

build/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build

-include $(CORE_OBJECTS:.o=.d) build/apps/cli/main.d build/apps/gui/main.d build/tests/test_main.d
