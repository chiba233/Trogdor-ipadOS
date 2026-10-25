PYTHON ?= python3
.PHONY: build test verify-sources
build:
	$(PYTHON) tools/build.py
test:
	$(PYTHON) -m unittest discover -s tests -v
verify-sources:
	$(PYTHON) tools/sources.py verify
