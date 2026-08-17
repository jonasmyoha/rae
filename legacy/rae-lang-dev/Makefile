.PHONY: all dev build test stop gemini llm up

SESSION_DIR ?= $(HOME)/.ws
PROJECT_KEY := rae-lang-dev
USER_KEY := $(shell id -un)
SESSION_LATEST := $(SESSION_DIR)/$(PROJECT_KEY).$(USER_KEY).latest
SESSION_HISTORY := $(SESSION_DIR)/$(PROJECT_KEY).$(USER_KEY).history.log

all: dev

dev:
	@$(MAKE) -C rae-devtools-web dev

build:
	@$(MAKE) -C rae/compiler build

test:
	@$(MAKE) -C rae/compiler test

stop:
	@$(MAKE) -C rae-devtools-web stop

llm:
	@mkdir -p "$(SESSION_DIR)"
	@cmd=""; \
	if command -v gemini >/dev/null 2>&1; then cmd="gemini --yolo -m gemini-3-flash-preview --resume latest"; fi; \
	if [ -z "$$cmd" ] && command -v claude >/dev/null 2>&1; then cmd="claude --continue"; fi; \
	if [ -z "$$cmd" ] && command -v codex >/dev/null 2>&1; then cmd="codex"; fi; \
	if [ -z "$$cmd" ]; then echo "No supported LLM CLI found (gemini/claude/codex)."; exit 1; fi; \
	printf "%s|%s|%s\n" "$$(date -Iseconds)" "$(PROJECT_KEY)" "$$cmd" >> "$(SESSION_HISTORY)"; \
	echo "$$cmd" > "$(SESSION_LATEST)"; \
	eval "$$cmd"

up:
	@echo "Run in two terminals for best UX: make dev + make llm"

gemini: llm
