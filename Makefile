CXX ?= c++
AR ?= ar
PKG_CONFIG ?= pkg-config
CURL_CONFIG ?= curl-config
KAIRO_USE_CURL ?= 1
UNAME_S := $(shell uname -s)
KAIRO_VERSION := $(strip $(shell sed -n '1p' VERSION))

.DELETE_ON_ERROR:

CPPFLAGS := -Icore/include -Icore/src -DKAIRO_VERSION=\"$(KAIRO_VERSION)\"
ifeq ($(UNAME_S),Haiku)
CPPFLAGS += -Ivendor/haiku-x86_64/include
endif
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDFLAGS += -pthread

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null || $(CURL_CONFIG) --cflags 2>/dev/null)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || $(CURL_CONFIG) --libs 2>/dev/null)
CRYPTO_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcrypto 2>/dev/null || $(PKG_CONFIG) --cflags openssl 2>/dev/null)
CRYPTO_LIBS := $(shell $(PKG_CONFIG) --libs libcrypto 2>/dev/null || $(PKG_CONFIG) --libs openssl 2>/dev/null)
CRYPTO_LINK_LIBS := $(if $(strip $(CRYPTO_LIBS)),$(CRYPTO_LIBS),-lcrypto)
CRYPTO_AVAILABLE := $(shell probe=/tmp/kairo-crypto-probe.$$$$; \
	printf '%s\n' 'int main(){OSSL_PARAM_BLD* b=OSSL_PARAM_BLD_new(); OSSL_PARAM_BLD_free(b); EVP_PKEY_CTX* c=EVP_PKEY_CTX_new_from_name(nullptr,"RSA",nullptr); EVP_PKEY_CTX_free(c);}' | \
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(CRYPTO_CFLAGS) -include openssl/evp.h -include openssl/param_build.h -x c++ - $(CRYPTO_LINK_LIBS) -o $$probe >/dev/null 2>&1 && printf 1; \
	rm -f $$probe)

# ChatGPT sign-in is a first-class GUI feature, so a Haiku GUI build must not
# silently compile it out. Keep `make clean` usable before the dependency is
# installed, while failing all targets that produce or verify the GUI.
REQUESTED_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
ifeq ($(UNAME_S),Haiku)
ifeq ($(KAIRO_USE_CURL),1)
ifneq ($(filter all gui check build/kairo-gui,$(REQUESTED_GOALS)),)
ifeq ($(CRYPTO_AVAILABLE),)
$(error OpenSSL 3 development files are required for ChatGPT sign-in. Run 'pkgman install devel:libcrypto pkgconfig', then rebuild with 'make clean && make gui')
endif
endif
endif
endif

ifeq ($(KAIRO_USE_CURL),1)
CPPFLAGS += -DKAIRO_HAS_CURL=1 -DKAIRO_HAS_CRYPTO=$(if $(CRYPTO_AVAILABLE),1,0) $(CURL_CFLAGS) $(if $(CRYPTO_AVAILABLE),$(CRYPTO_CFLAGS))
PROVIDER_LIBS := $(if $(strip $(CURL_LIBS)),$(CURL_LIBS),-lcurl) $(if $(CRYPTO_AVAILABLE),$(CRYPTO_LINK_LIBS))
else
CPPFLAGS += -DKAIRO_HAS_CURL=0 -DKAIRO_HAS_CRYPTO=0
endif

CORE_SOURCES := \
	core/src/agent_engine.cpp \
	core/src/chatgpt_auth.cpp \
	core/src/chatgpt_provider.cpp \
	core/src/diagnostics.cpp \
	core/src/events.cpp \
	core/src/json.cpp \
	core/src/model.cpp \
	core/src/openai_provider.cpp \
	core/src/provider_profile.cpp \
	core/src/session_store.cpp \
	core/src/sse_decoder.cpp \
	core/src/workspace.cpp
CORE_OBJECTS := $(CORE_SOURCES:%.cpp=build/%.o)

PROVIDER_RPATH :=
BUNDLED_CURL_OUTPUTS :=
ifeq ($(UNAME_S),Haiku)
ifeq ($(KAIRO_USE_CURL),1)
BUNDLED_CURL_DIR := vendor/haiku-x86_64/lib
BUNDLED_CURL_OUTPUTS := build/libcurl.so build/libcurl.so.4 build/libcurl.so.4.8.0
PROVIDER_LIBS := -L$(BUNDLED_CURL_DIR) -lcurl $(if $(CRYPTO_AVAILABLE),$(CRYPTO_LINK_LIBS)) -lnetwork
PROVIDER_RPATH := -Wl,-rpath,'$$ORIGIN'
endif
endif

.PHONY: all clean test check gui
all: build/kairo-cli $(if $(filter Haiku,$(UNAME_S)),build/kairo-gui)

build/libkairo.a: $(CORE_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

build/kairo-cli: build/apps/cli/main.o build/libkairo.a $(BUNDLED_CURL_OUTPUTS)
	$(CXX) $(LDFLAGS) $(PROVIDER_RPATH) -o $@ build/apps/cli/main.o build/libkairo.a $(PROVIDER_LIBS)

build/kairo-tests: build/tests/test_main.o build/libkairo.a $(BUNDLED_CURL_OUTPUTS)
	$(CXX) $(LDFLAGS) $(PROVIDER_RPATH) -o $@ build/tests/test_main.o build/libkairo.a $(PROVIDER_LIBS)

build/kairo-gui: build/apps/gui/main.o build/libkairo.a $(BUNDLED_CURL_OUTPUTS) \
		VERSION resources/kairo.rdef resources/kairo-icon.svg scripts/embed-haiku-resources.sh
	$(CXX) $(LDFLAGS) $(PROVIDER_RPATH) -o $@ build/apps/gui/main.o build/libkairo.a $(PROVIDER_LIBS) -lbe -ltracker
	./scripts/embed-haiku-resources.sh $@

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

build/apps/gui/main.o: VERSION

clean:
	rm -rf build

-include $(CORE_OBJECTS:.o=.d) build/apps/cli/main.d build/apps/gui/main.d build/tests/test_main.d
