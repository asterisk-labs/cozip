# cozip top-level Makefile.

VERSION := $(shell tr -d '[:space:]' < VERSION)

CORE_DIR   := core
PY_DIR     := python
R_DIR      := r
JL_DIR     := julia
JS_DIR     := javascript
BUILD_DIR  := $(CORE_DIR)/build
PY_LIB_DIR := $(PY_DIR)/cozip/_lib
DIST_DIR   := dist
CMAKE      ?= cmake
PYTHON     ?= python3

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

# All packages use VERSION as CalVer YYYY.M.D, without leading zeros.
CALVER_RE := ^[0-9]{4}\.([1-9]|1[0-2])\.([1-9]|[12][0-9]|3[01])$$

# Prints the JSON value at a key path: $(JSON_GET) file key [key ...].
JSON_GET := $(PYTHON) -c 'import functools, json, sys; print(functools.reduce(lambda v, k: v[k], sys.argv[2:], json.load(open(sys.argv[1], encoding="utf-8"))))'

# Sets the root and packages[""] versions of a package-lock.json.
JSON_SET_LOCK_VERSION := $(PYTHON) -c 'import json, sys; f, v = sys.argv[1:]; d = json.load(open(f, encoding="utf-8")); d["version"] = d["packages"][""]["version"] = v; open(f, "w", encoding="utf-8").write(json.dumps(d, indent=2, ensure_ascii=False) + "\n")'

# Build objects in r/src are not drift.
DIFF_EXCLUDE := -x '*.o' -x .DS_Store

.PHONY: all lib check-version check sync python r julia javascript docs clean help

all: sync lib python r julia javascript
	@echo "cozip $(VERSION): all green"

help:
	@echo "make {all|check|sync|lib|python|r|julia|javascript|docs|clean}"

lib:
	$(CMAKE) -B $(BUILD_DIR) -S $(CORE_DIR) -G Ninja
	$(CMAKE) --build $(BUILD_DIR)
	mkdir -p $(PY_LIB_DIR)
	cp $(BUILD_DIR)/$(LIB_NAME) $(PY_LIB_DIR)/$(LIB_NAME)

check-version:
	@printf '%s\n' '$(VERSION)' | grep -Eq '$(CALVER_RE)' || \
	  { echo "VERSION '$(VERSION)' is not CalVer YYYY.M.D without leading zeros (for example 2026.9.27)" >&2; exit 1; }

# Check version and synced copies without writing.
check: check-version
	@fail=0; \
	drift() { echo "check: $$*" >&2; fail=1; }; \
	same() { [ "$$2" = "$(VERSION)" ] || drift "$$1 has version '$$2', VERSION is $(VERSION)"; }; \
	same $(R_DIR)/DESCRIPTION "$$(sed -n 's/^Version:[[:space:]]*//p' $(R_DIR)/DESCRIPTION)"; \
	same $(JL_DIR)/Project.toml "$$(sed -n -E 's/^version = "(.*)"/\1/p' $(JL_DIR)/Project.toml)"; \
	same $(JS_DIR)/package.json "$$($(JSON_GET) $(JS_DIR)/package.json version)"; \
	same $(JS_DIR)/package-lock.json "$$($(JSON_GET) $(JS_DIR)/package-lock.json version)"; \
	same '$(JS_DIR)/package-lock.json packages[""]' "$$($(JSON_GET) $(JS_DIR)/package-lock.json packages '' version)"; \
	cmp -s VERSION $(PY_DIR)/VERSION || drift "$(PY_DIR)/VERSION differs from VERSION"; \
	for f in $(PY_DIR)/LICENSE $(JS_DIR)/LICENSE; do \
	  cmp -s LICENSE $$f || drift "$$f differs from LICENSE"; \
	done; \
	for f in cozip.c cozip.h cozip_read.c karu/VERSION karu/LICENSE; do \
	  cmp -s $(CORE_DIR)/$$f $(R_DIR)/src/$$f || drift "$(R_DIR)/src/$$f differs from $(CORE_DIR)/$$f"; \
	done; \
	for d in libzip zlib karu/include karu/src; do \
	  diff -r -q $(DIFF_EXCLUDE) $(CORE_DIR)/$$d $(R_DIR)/src/$$d >/dev/null || drift "$(R_DIR)/src/$$d/ differs from $(CORE_DIR)/$$d/"; \
	done; \
	[ $$fail -eq 0 ] || { echo "check: run make sync to refresh the copies" >&2; exit 1; }; \
	echo "check OK $(VERSION)"

# Copies VERSION, LICENSE and core/ into the packages, then runs check.
sync: check-version
	@cp VERSION $(PY_DIR)/VERSION
	@cp LICENSE $(PY_DIR)/LICENSE
	@cp LICENSE $(JS_DIR)/LICENSE
	@sed -i.bak -E 's/^Version:.*/Version: $(VERSION)/' $(R_DIR)/DESCRIPTION
	@rm -f $(R_DIR)/DESCRIPTION.bak
	@sed -i.bak -E 's/^version = ".*"/version = "$(VERSION)"/' $(JL_DIR)/Project.toml
	@rm -f $(JL_DIR)/Project.toml.bak
	@sed -i.bak -E 's/^(  "version": )"[^"]*"/\1"$(VERSION)"/' $(JS_DIR)/package.json
	@rm -f $(JS_DIR)/package.json.bak
	@$(JSON_SET_LOCK_VERSION) $(JS_DIR)/package-lock.json $(VERSION)
	@rm -f $(R_DIR)/src/cozip.c $(R_DIR)/src/cozip.h $(R_DIR)/src/cozip_read.c
	@cp $(CORE_DIR)/cozip.c $(R_DIR)/src/cozip.c
	@cp $(CORE_DIR)/cozip.h $(R_DIR)/src/cozip.h
	@cp $(CORE_DIR)/cozip_read.c $(R_DIR)/src/cozip_read.c
	@rsync -a --delete $(CORE_DIR)/libzip/ $(R_DIR)/src/libzip/
	@rsync -a --delete $(CORE_DIR)/zlib/   $(R_DIR)/src/zlib/
	@mkdir -p $(R_DIR)/src/karu
	@rsync -a --delete $(CORE_DIR)/karu/include/ $(R_DIR)/src/karu/include/
	@rsync -a --delete $(CORE_DIR)/karu/src/     $(R_DIR)/src/karu/src/
	@cp $(CORE_DIR)/karu/VERSION $(CORE_DIR)/karu/LICENSE $(R_DIR)/src/karu/
	@$(MAKE) --no-print-directory check

python: lib
	$(PYTHON) -c 'import pytest, build' || \
	  { echo "missing pytest or build: $(PYTHON) -m pip install pytest build" >&2; exit 1; }
	rm -rf $(PY_DIR)/dist $(PY_DIR)/build $(PY_DIR)/*.egg-info
	$(PYTHON) -m pip install -e $(PY_DIR)
	$(PYTHON) -m pytest $(PY_DIR)/tests -v
	$(PYTHON) -m build --wheel $(PY_DIR)

# Stale .so from a different OS survives make's timestamp check.
# r/configure writes src/version.h from DESCRIPTION.
r: sync
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
	   using Pkg; Pkg.resolve(); Pkg.instantiate(); Pkg.test()'

# Pure-JS package: no native lib, but version sync is required.
javascript: sync
	@command -v npm >/dev/null || { echo "missing npm"; exit 1; }
	cd $(JS_DIR) && npm install
	cd $(JS_DIR) && npm run types
	cd $(JS_DIR) && npm test
	mkdir -p $(DIST_DIR)
	cd $(JS_DIR) && npm pack --pack-destination ../$(DIST_DIR)

# Build the static site from docs/ into the disposable _site directory.
docs:
	$(PYTHON) tools/build_docs.py --output _site --clean

clean:
	rm -rf $(CORE_DIR)/build/ $(CORE_DIR)/build-*/ $(DIST_DIR)/ _site/
	rm -f $(PY_LIB_DIR)/*.dylib $(PY_LIB_DIR)/*.so $(PY_LIB_DIR)/*.dll
	rm -f $(R_DIR)/src/version.h
	rm -f $(R_DIR)/src/*.so $(R_DIR)/src/*.dylib $(R_DIR)/src/*.dll
	@find $(R_DIR)/src -name '*.o' -delete
	rm -f $(R_DIR)/src/.DS_Store
	rm -f $(R_DIR)/src/Makevars $(R_DIR)/src/Makevars.win
	rm -rf $(PY_DIR)/dist $(PY_DIR)/build $(PY_DIR)/*.egg-info
	rm -rf $(JS_DIR)/node_modules $(JS_DIR)/types
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
