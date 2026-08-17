.PHONY: all dev build test stop setup devtools-install devtools-lint devtools-test gemini claude codex llm up

SESSION_DIR ?= $(HOME)/.ws
PROJECT_KEY := rae
USER_KEY := $(shell id -un)
SESSION_LATEST := $(SESSION_DIR)/$(PROJECT_KEY).$(USER_KEY).latest
SESSION_HISTORY := $(SESSION_DIR)/$(PROJECT_KEY).$(USER_KEY).history.log

all: dev

dev:
	@$(MAKE) -C tools/devtools-web dev

build:
	@$(MAKE) -C compiler build

test:
	@$(MAKE) -C compiler test

stop:
	@$(MAKE) -C tools/devtools-web stop

setup:
	@./setup.sh

devtools-install:
	@cd tools/devtools-web && bun install --frozen-lockfile

devtools-lint:
	@cd tools/devtools-web && bun run lint

devtools-test:
	@cd tools/devtools-web && bun run test:runner

llm:
	@mkdir -p "$(SESSION_DIR)"
	@cmd=""; \
	if command -v claude >/dev/null 2>&1; then cmd="claude --dangerously-skip-permissions --continue"; fi; \
	if [ -z "$$cmd" ] && command -v codex >/dev/null 2>&1; then cmd="codex"; fi; \
	if [ -z "$$cmd" ] && command -v gemini >/dev/null 2>&1; then cmd="gemini --yolo -m gemini-3-flash-preview --resume latest"; fi; \
	if [ -z "$$cmd" ]; then echo "No supported LLM CLI found (claude/codex/gemini)."; exit 1; fi; \
	printf "%s|%s|%s\n" "$$(date -Iseconds)" "$(PROJECT_KEY)" "$$cmd" >> "$(SESSION_HISTORY)"; \
	echo "$$cmd" > "$(SESSION_LATEST)"; \
	eval "$$cmd"

up:
	@echo "Run in two terminals for best UX: make dev + make llm"

# Explicit per-agent launchers. Same commands + session recording as `llm`,
# but each forces a specific CLI. `make llm` still auto-picks the first found.
gemini:
	@mkdir -p "$(SESSION_DIR)"
	@cmd="gemini --yolo -m gemini-3-flash-preview --resume latest"; \
	command -v gemini >/dev/null 2>&1 || { echo "gemini CLI not found."; exit 1; }; \
	printf "%s|%s|%s\n" "$$(date -Iseconds)" "$(PROJECT_KEY)" "$$cmd" >> "$(SESSION_HISTORY)"; \
	echo "$$cmd" > "$(SESSION_LATEST)"; \
	eval "$$cmd"

claude:
	@mkdir -p "$(SESSION_DIR)"
	@cmd="claude --dangerously-skip-permissions --continue"; \
	command -v claude >/dev/null 2>&1 || { echo "claude CLI not found."; exit 1; }; \
	printf "%s|%s|%s\n" "$$(date -Iseconds)" "$(PROJECT_KEY)" "$$cmd" >> "$(SESSION_HISTORY)"; \
	echo "$$cmd" > "$(SESSION_LATEST)"; \
	eval "$$cmd"

codex:
	@mkdir -p "$(SESSION_DIR)"
	@cmd="codex"; \
	command -v codex >/dev/null 2>&1 || { echo "codex CLI not found."; exit 1; }; \
	printf "%s|%s|%s\n" "$$(date -Iseconds)" "$(PROJECT_KEY)" "$$cmd" >> "$(SESSION_HISTORY)"; \
	echo "$$cmd" > "$(SESSION_LATEST)"; \
	eval "$$cmd"
