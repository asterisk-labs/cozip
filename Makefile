# cozip top-level Makefile.
#
# Targets:
#   all      sync + lib + python + r + julia (full pre-push validation)
#   sync     bump VERSION into r/julia, mirror core C into r/src/
#   lib      build libcozip, install into python/_lib/
#   wasm     build cozip.wasm + cozip.js
#   python   clean + install + test + wheel
#   r        clean + install + build + check
#   julia    instantiate + test
#   clean    nuke build artifacts
#
# Layout:
#   core/build/    libcozip (cmake + ninja, vendored libzip + zlib)
#   python/build/  wheel
#   dist/          R tarballs

VERSION    := $(shell tr -d '[:space:]' < VERSION)
VERSION_JL := $(shell echo $(VERSION) | cut -d. -f1-3 | tr -d '[:space:]')

CORE_DIR   := core
PY_DIR     := python
R_DIR      := r
JL_DIR     := julia
BUILD_DIR  := $(CORE_DIR)/build
PY_LIB_DIR := $(PY_DIR)/cozip/_lib
DIST_DIR   := dist
CMAKE      ?= cmake
EMCMAKE    ?= emcmake
EMCC       ?= emcc

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LIB_NAME := cozip.dylib
endif
ifeq ($(UNAME_S),Linux)
    LIB_NAME := cozip.so
endif
ifeq ($(OS),Windows_NT)
    LIB_NAME := cozip.dll
endif

WASM_BUILD_DIR := $(CORE_DIR)/build-wasm
JS_WASM_DIR    := javascript/wasm

.PHONY: all lib wasm sync python r julia clean help

all: sync lib python r julia
	@echo ""
	@echo "all green: sync + lib + python + r + julia"
	@echo "  cozip $(VERSION)"

help:
	@echo "cozip $(VERSION)"
	@echo "  Vendored: libzip 1.11.4, zlib 1.3.1"
	@echo ""
	@echo "  all      full validation (sync + lib + python + r + julia)"
	@echo "  sync     VERSION into r/julia + core C into r/src/"
	@echo "  lib      build libcozip + install into python/_lib/"
	@echo "  wasm     build cozip.wasm + cozip.js"
	@echo "  python   clean + install + test + wheel"
	@echo "  r        clean + install + build + check"
	@echo "  julia    instantiate + test"
	@echo "  clean    nuke build artifacts"

# --- C library ---

lib:
	$(CMAKE) -B $(BUILD_DIR) -S $(CORE_DIR) -G Ninja
	$(CMAKE) --build $(BUILD_DIR)
	mkdir -p $(PY_LIB_DIR)
	cp $(BUILD_DIR)/$(LIB_NAME) $(PY_LIB_DIR)/$(LIB_NAME)

# --- WASM ---
#
# cozip + libzip + zlib bundled into a single WebAssembly module
# loadable from Node or browser. Output goes to javascript/wasm/
# (not committed). Needs Emscripten 3.x+. Exports list must match
# cozip.h.

wasm:
	$(EMCMAKE) $(CMAKE) -B $(WASM_BUILD_DIR) -S $(CORE_DIR) -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
	$(CMAKE) --build $(WASM_BUILD_DIR)
	mkdir -p $(JS_WASM_DIR)
	$(EMCC) \
		-I $(CORE_DIR) -I $(WASM_BUILD_DIR) \
		javascript/wasm_bridge.c \
		$(WASM_BUILD_DIR)/cozip.a \
		$(WASM_BUILD_DIR)/libzip/libzip.a \
		$(WASM_BUILD_DIR)/zlib/libzlibstatic.a \
		--no-entry \
		-sEXPORTED_FUNCTIONS='["_cozip_version_string","_cozip_status_string","_cozip_plan","_cozip_index_payload_size","_cozip_build_index_payload","_cozip_build_extra_field","_cozip_write_archive","_cozip_patch_integrity_hash","_cozip_predict_zip32_archive_size","_cozip_required_padding_payload","_cozip_wasm_error_size","_cozip_wasm_error_message_offset","_cozip_wasm_write_archive_from_buffers","_malloc","_free"]' \
		-sEXPORTED_RUNTIME_METHODS='["FS","UTF8ToString","stringToUTF8","lengthBytesUTF8","HEAPU8","HEAPU32"]' \
		-sFORCE_FILESYSTEM=1 -sMODULARIZE -sEXPORT_ES6=0 \
		-sEXPORT_NAME=createCozipModule -sALLOW_MEMORY_GROWTH=1 \
		-o $(JS_WASM_DIR)/cozip.js -O2
	@echo "WASM: $(JS_WASM_DIR)/cozip.{js,wasm}"

# --- sync ---
#
# 4-part CalVer (e.g. 2026.5.2.6) goes into r/DESCRIPTION (full)
# and julia/Project.toml (3 parts, Julia wants strict SemVer).
# Mirrors core/cozip.{c,h} + libzip/ + zlib/ into r/src/. Julia
# pulls libcozip via Pkg.Artifacts, so no C mirror there.
# Last step verifies coherence and bails if anything drifted.

sync:
	@sed -i.bak -E 's/^Version:.*/Version: $(VERSION)/' $(R_DIR)/DESCRIPTION
	@rm -f $(R_DIR)/DESCRIPTION.bak
	@echo "DESCRIPTION: Version $(VERSION)"
	@sed -i.bak -E 's/^version = ".*"/version = "$(VERSION_JL)"/' $(JL_DIR)/Project.toml
	@rm -f $(JL_DIR)/Project.toml.bak
	@echo "Project.toml: version $(VERSION_JL)"
	@rm -f $(R_DIR)/src/cozip.c $(R_DIR)/src/cozip.h
	@cp $(CORE_DIR)/cozip.c $(R_DIR)/src/cozip.c
	@cp $(CORE_DIR)/cozip.h $(R_DIR)/src/cozip.h
	@rsync -a --delete $(CORE_DIR)/libzip/ $(R_DIR)/src/libzip/
	@rsync -a --delete $(CORE_DIR)/zlib/   $(R_DIR)/src/zlib/
	@echo "core/cozip.{c,h} + libzip/ + zlib/ -> r/src/"
	@VFILE=$$(grep -E '^Version:' $(R_DIR)/DESCRIPTION | sed 's/Version:[[:space:]]*//'); \
	 [ "$$VFILE" = "$(VERSION)" ] || { echo "check: DESCRIPTION=$$VFILE != $(VERSION)"; exit 1; }
	@VJL=$$(grep -E '^version = ' $(JL_DIR)/Project.toml | sed -E 's/version = "(.*)"/\1/'); \
	 [ "$$VJL" = "$(VERSION_JL)" ] || { echo "check: Project.toml=$$VJL != $(VERSION_JL)"; exit 1; }
	@diff -q $(CORE_DIR)/cozip.c $(R_DIR)/src/cozip.c >/dev/null || { echo "check: cozip.c drift"; exit 1; }
	@diff -q $(CORE_DIR)/cozip.h $(R_DIR)/src/cozip.h >/dev/null || { echo "check: cozip.h drift"; exit 1; }
	@diff -r -q $(CORE_DIR)/libzip $(R_DIR)/src/libzip >/dev/null || { echo "check: libzip/ drift"; exit 1; }
	@diff -r -q $(CORE_DIR)/zlib $(R_DIR)/src/zlib >/dev/null     || { echo "check: zlib/ drift"; exit 1; }
	@echo "sync OK"

# --- Python ---

python: lib
	@python -c 'import pytest, build' 2>/dev/null || \
	  { echo "ERROR: pytest/build missing, run: pip install pytest build"; exit 1; }
	rm -rf $(PY_DIR)/dist $(PY_DIR)/build $(PY_DIR)/*.egg-info
	pip install -e $(PY_DIR)
	cd $(PY_DIR) && pytest tests/ -v
	cd $(PY_DIR) && python -m build --wheel

# --- R ---
#
# Wipe stale artifacts in r/src/ before compiling. make only checks
# timestamps, so an old Mach-O .so can survive a Linux build and
# blow up at load with "invalid ELF header".

r: sync
	rm -f $(R_DIR)/src/version.h
	rm -f $(R_DIR)/src/*.so $(R_DIR)/src/*.dylib $(R_DIR)/src/*.dll
	@find $(R_DIR)/src -name '*.o' -delete
	cd $(R_DIR) && Rscript -e 'roxygen2::roxygenise()'
	R CMD INSTALL $(R_DIR)
	mkdir -p $(DIST_DIR)
	cd $(DIST_DIR) && R CMD build ../$(R_DIR)
	cd $(DIST_DIR) && _R_CHECK_FORCE_SUGGESTS_=false R CMD check cozip_$(VERSION).tar.gz

# --- Julia ---
#
# Pulls libcozip via Pkg.Artifacts (downloaded from the GH Release).
# Project.toml uses strict SemVer (3 components); the 4th component
# of the C side is exposed via LibCozip.cozip_version().
#
# `julia` depends on `lib` so libcozip is built before the tests
# run. COZIP_LIB_PATH is set with ENV[...] inside the parent Julia
# process: Pkg.test() forks a subprocess with its own environment,
# and that's the only way the var gets inherited reliably. abspath
# because the child changes cwd.
#
# This way tests pass even without a valid Artifacts.toml yet:
# LibCozip.jl does a runtime lookup and falls back to the ENV var
# when the artifact isn't registered.

julia: lib
	cd $(JL_DIR) && julia --project=. -e \
	  'ENV["COZIP_LIB_PATH"] = "$(abspath $(PY_LIB_DIR)/$(LIB_NAME))"; \
	   using Pkg; Pkg.instantiate(); Pkg.test()'

# --- Clean ---

clean:
	rm -rf $(CORE_DIR)/build/ $(CORE_DIR)/build-wasm/ $(DIST_DIR)/
	rm -rf $(JS_WASM_DIR)/
	rm -f $(PY_LIB_DIR)/*.dylib $(PY_LIB_DIR)/*.so $(PY_LIB_DIR)/*.dll
	rm -f $(R_DIR)/src/version.h
	rm -f $(R_DIR)/src/*.so $(R_DIR)/src/*.dylib $(R_DIR)/src/*.dll
	@find $(R_DIR)/src -name '*.o' -delete
	rm -f $(R_DIR)/src/.DS_Store
	rm -rf $(PY_DIR)/dist $(PY_DIR)/build $(PY_DIR)/*.egg-info
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true