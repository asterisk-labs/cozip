# cozip top-level Makefile.

VERSION     := $(shell tr -d '[:space:]' < VERSION)
VERSION_JL  := $(shell echo $(VERSION) | cut -d. -f1-3 | tr -d '[:space:]')
VERSION_NPM := $(VERSION_JL)

CORE_DIR   := core
PY_DIR     := python
R_DIR      := r
JL_DIR     := julia
JS_DIR     := javascript
BUILD_DIR  := $(CORE_DIR)/build
PY_LIB_DIR := $(PY_DIR)/cozip/_lib
DIST_DIR   := dist
CMAKE      ?= cmake

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

.PHONY: all lib sync python r julia javascript clean help

all: sync lib python r julia javascript
	@echo "cozip $(VERSION): all green"

help:
	@echo "make {all|sync|lib|python|r|julia|javascript|clean}"

lib:
	$(CMAKE) -B $(BUILD_DIR) -S $(CORE_DIR) -G Ninja
	$(CMAKE) --build $(BUILD_DIR)
	mkdir -p $(PY_LIB_DIR)
	cp $(BUILD_DIR)/$(LIB_NAME) $(PY_LIB_DIR)/$(LIB_NAME)

# 4-part CalVer in R, 3-part SemVer in Julia and npm.
sync:
	@sed -i.bak -E 's/^Version:.*/Version: $(VERSION)/' $(R_DIR)/DESCRIPTION
	@rm -f $(R_DIR)/DESCRIPTION.bak
	@sed -i.bak -E 's/^version = ".*"/version = "$(VERSION_JL)"/' $(JL_DIR)/Project.toml
	@rm -f $(JL_DIR)/Project.toml.bak
	@sed -i.bak -E 's/("version": )"[^"]*"/\1"$(VERSION_NPM)"/' $(JS_DIR)/package.json
	@rm -f $(JS_DIR)/package.json.bak
	@rm -f $(R_DIR)/src/cozip.c $(R_DIR)/src/cozip.h
	@cp $(CORE_DIR)/cozip.c $(R_DIR)/src/cozip.c
	@cp $(CORE_DIR)/cozip.h $(R_DIR)/src/cozip.h
	@rsync -a --delete $(CORE_DIR)/libzip/ $(R_DIR)/src/libzip/
	@rsync -a --delete $(CORE_DIR)/zlib/   $(R_DIR)/src/zlib/
	@VFILE=$$(grep -E '^Version:' $(R_DIR)/DESCRIPTION | sed 's/Version:[[:space:]]*//'); \
	 [ "$$VFILE" = "$(VERSION)" ] || { echo "check: DESCRIPTION=$$VFILE != $(VERSION)"; exit 1; }
	@VJL=$$(grep -E '^version = ' $(JL_DIR)/Project.toml | sed -E 's/version = "(.*)"/\1/'); \
	 [ "$$VJL" = "$(VERSION_JL)" ] || { echo "check: Project.toml=$$VJL != $(VERSION_JL)"; exit 1; }
	@VJS=$$(grep -E '"version":' $(JS_DIR)/package.json | head -1 | sed -E 's/.*"version": "([^"]+)".*/\1/'); \
	 [ "$$VJS" = "$(VERSION_NPM)" ] || { echo "check: package.json=$$VJS != $(VERSION_NPM)"; exit 1; }
	@diff -q $(CORE_DIR)/cozip.c $(R_DIR)/src/cozip.c >/dev/null || { echo "check: cozip.c drift"; exit 1; }
	@diff -q $(CORE_DIR)/cozip.h $(R_DIR)/src/cozip.h >/dev/null || { echo "check: cozip.h drift"; exit 1; }
	@diff -r -q $(CORE_DIR)/libzip $(R_DIR)/src/libzip >/dev/null || { echo "check: libzip/ drift"; exit 1; }
	@diff -r -q $(CORE_DIR)/zlib $(R_DIR)/src/zlib >/dev/null     || { echo "check: zlib/ drift"; exit 1; }
	@echo "sync OK $(VERSION)"

python: lib
	@python -c 'import pytest, build' 2>/dev/null || \
	  { echo "missing pytest/build: pip install pytest build"; exit 1; }
	rm -rf $(PY_DIR)/dist $(PY_DIR)/build $(PY_DIR)/*.egg-info
	pip install -e $(PY_DIR)
	cd $(PY_DIR) && pytest tests/ -v
	cd $(PY_DIR) && python -m build --wheel

# Stale .so from a different OS survives make's timestamp check.
r: sync
	rm -f $(R_DIR)/src/version.h
	rm -f $(R_DIR)/src/*.so $(R_DIR)/src/*.dylib $(R_DIR)/src/*.dll
	@find $(R_DIR)/src -name '*.o' -delete
	cd $(R_DIR) && Rscript -e 'roxygen2::roxygenise()'
	R CMD INSTALL $(R_DIR)
	mkdir -p $(DIST_DIR)
	cd $(DIST_DIR) && R CMD build ../$(R_DIR)
	cd $(DIST_DIR) && _R_CHECK_FORCE_SUGGESTS_=false R CMD check cozip_$(VERSION).tar.gz

# Pkg.test() forks a subprocess; ENV[] in the parent is the only reliable way to pass COZIP_LIB_PATH.
julia: lib
	cd $(JL_DIR) && julia --project=. -e \
	  'ENV["COZIP_LIB_PATH"] = "$(abspath $(PY_LIB_DIR)/$(LIB_NAME))"; \
	   using Pkg; Pkg.instantiate(); Pkg.test()'

# Pure-JS package: no native lib, but version sync is required.
javascript: sync
	@command -v npm >/dev/null || { echo "missing npm"; exit 1; }
	cd $(JS_DIR) && npm install
	cd $(JS_DIR) && npm run types
	cd $(JS_DIR) && npm test
	mkdir -p $(DIST_DIR)
	cd $(JS_DIR) && npm pack --pack-destination ../$(DIST_DIR)

clean:
	rm -rf $(CORE_DIR)/build/ $(DIST_DIR)/
	rm -f $(PY_LIB_DIR)/*.dylib $(PY_LIB_DIR)/*.so $(PY_LIB_DIR)/*.dll
	rm -f $(R_DIR)/src/version.h
	rm -f $(R_DIR)/src/*.so $(R_DIR)/src/*.dylib $(R_DIR)/src/*.dll
	@find $(R_DIR)/src -name '*.o' -delete
	rm -f $(R_DIR)/src/.DS_Store
	rm -rf $(PY_DIR)/dist $(PY_DIR)/build $(PY_DIR)/*.egg-info
	rm -rf $(JS_DIR)/node_modules $(JS_DIR)/types
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true