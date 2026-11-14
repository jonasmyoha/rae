.PHONY: all dev build test stop setup install-hooks banned-terms-gate example-copy-gate stress bench-servers devtools-install devtools-lint devtools-test gemini claude codex llm up

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

# Visual example gates (windows + screenshots). Not part of `make test`.
test-examples:
	@$(MAKE) -C compiler test-examples

stop:
	@$(MAKE) -C tools/devtools-web stop

# First-time setup: Devtools Web dependencies (when Bun is installed), the
# compiler, naga, upstream Box3D's C library (lib/box3d links it) and WAMR
# (lib/wasm links it; rebuilt when its pin or Rae's glue moved). A package's C builds
# when a program first needs it.
setup:
	@if command -v git-lfs >/dev/null 2>&1; then \
	  echo "git-lfs found: the binary files (images, fonts, sounds) come from Git LFS"; \
	elif [ "$$(uname)" = "Darwin" ] && command -v brew >/dev/null 2>&1; then \
	  echo "Installing git-lfs (the binary files are in Git LFS)..."; \
	  brew install git-lfs; \
	else \
	  echo "warning: git-lfs is not installed; the images, fonts and sounds stay LFS pointers (install git-lfs, then: make install-hooks)" >&2; \
	fi
	@$(MAKE) install-hooks
	@if command -v bun >/dev/null 2>&1; then \
	  echo "Installing Devtools Web dependencies..."; \
	  (cd tools/devtools-web && bun install --frozen-lockfile); \
	else \
	  echo "warning: Bun is not installed; skipping Devtools Web dependencies (install Bun, then: make devtools-install)" >&2; \
	fi
	@$(MAKE) -C compiler build
	@if command -v naga >/dev/null 2>&1 || [ -x "$$HOME/.cargo/bin/naga" ]; then \
	  echo "naga found: declared shaders are validated at build time"; \
	elif command -v cargo >/dev/null 2>&1; then \
	  echo "Installing naga-cli (the WGSL validator shader(files:) runs at build time)..."; \
	  cargo install naga-cli; \
	else \
	  echo "warning: naga not installed and cargo is not available; declared shaders will not be validated at build time (install Rust, then: cargo install naga-cli)" >&2; \
	fi
	@if [ -x /opt/homebrew/opt/llvm/bin/clang ] || [ -x /usr/local/opt/llvm/bin/clang ] || [ -n "$${RAE_RTSAN_CC:-}" ]; then \
	  echo "LLVM clang found: rae run --sanitize realtime and the RealtimeSanitizer gate can run"; \
	elif [ "$$(uname)" = "Darwin" ] && command -v brew >/dev/null 2>&1; then \
	  echo "Installing LLVM (Apple's clang has no RealtimeSanitizer; docs/realtime-language-design.md 5.2)..."; \
	  brew install llvm; \
	else \
	  echo "warning: no LLVM clang with -fsanitize=realtime; the RealtimeSanitizer gate will SKIP (install LLVM 20+, or set RAE_RTSAN_CC)" >&2; \
	fi
	@if [ -f "$${RAE_BOX3D:-$$HOME/.cache/rae/box3d/install}/include/box3d/rae_glue.h" ]; then \
	  echo "box3d found: programs importing lib/box3d link upstream Box3D's C library"; \
	else \
	  echo "Building upstream Box3D (the C-library physics track; tools/box3d/build.sh)..."; \
	  bash tools/box3d/build.sh; \
	fi
	@wamr="$${RAE_WAMR:-$$HOME/.cache/rae/wamr/install}"; \
	if [ -f "$$wamr/lib/librae_wamr.a" ] \
	  && [ "$$(cat "$$wamr/COMMIT" 2>/dev/null)" = "$$(. tools/wamr/pin; echo $$WAMR_COMMIT)" ] \
	  && [ ! tools/wamr/rae_wasm_glue.c -nt "$$wamr/lib/librae_wamr.a" ] \
	  && [ ! compiler/runtime/runtime_wasm_embed.h -nt "$$wamr/lib/librae_wamr.a" ]; then \
	  echo "wamr found: programs importing lib/wasm link WAMR's fast interpreter"; \
	else \
	  echo "Building WAMR (lib/wasm; tools/wamr/build.sh)..."; \
	  bash tools/wamr/build.sh; \
	fi
	@echo "Setup complete. Compiler: ./compiler/bin/rae   Devtools Web: make dev"

# The banned-terms gate (#1016) on its own: every tracked text file plus the
# messages of origin/main..HEAD, against the term list OUTSIDE the repo
# ($RAE_BANNED_TERMS_FILE, default ~/.config/rae/banned-terms.txt). The full
# suite runs it too; this is the quick standalone form.
banned-terms-gate:
	@bash compiler/tools/banned-terms-gate.sh

# Example files that copy another example's file or a lib/ file, outside
# compiler/tools/example-copy-allowlist.txt (AGENTS.md "Example families").
# The full test run includes it as one pre-suite case.
example-copy-gate:
	@bash compiler/tools/example-copy-gate.sh

# The stress cases (docs/stress-tests.md): every stress/NN_*/ program held to
# the verdict its pack declares — a "handles" case must show the good
# outcome, a "footgun" case must still show the bad one (FIXED? fails: flip
# the case). Seconds; the full suite runs it too. RAE_STRESS_FILTER scopes it.
stress:
	@$(MAKE) -C compiler build
	@bash stress/run.sh

# The server benchmarks (benchmarks/servers, docs/server-benchmarks-design.md):
# builds the pinned load tools into ~/.cache/rae/servers (once), then runs every
# server under load and rewrites benchmarks/servers/results/. About 40 minutes
# on an idle machine; it refuses to start on a busy one (BENCH_ALLOW_LOAD=1).
bench-servers:
	@$(MAKE) -C compiler build
	@sh benchmarks/servers/fetch.sh
	@sh benchmarks/servers/run.sh

# Opt-in git hooks (#1016): pre-commit checks the STAGED changes, commit-msg
# the message, both against the same list. Git never versions .git/hooks, so
# each clone installs them on purpose; idempotent (symlinks, so they track
# the repo's copy).
install-hooks:
	@ln -sfn "$(CURDIR)/tools/git-hooks/pre-commit" .git/hooks/pre-commit
	@ln -sfn "$(CURDIR)/tools/git-hooks/commit-msg" .git/hooks/commit-msg
	@ln -sfn "$(CURDIR)/tools/git-hooks/post-commit" .git/hooks/post-commit
	@ln -sfn "$(CURDIR)/tools/git-hooks/post-merge" .git/hooks/post-merge
	@ln -sfn "$(CURDIR)/tools/git-hooks/post-rewrite" .git/hooks/post-rewrite
	@ln -sfn "$(CURDIR)/tools/git-hooks/pre-push" .git/hooks/pre-push
	@ln -sfn "$(CURDIR)/tools/git-hooks/post-checkout" .git/hooks/post-checkout
	@if command -v git-lfs >/dev/null 2>&1; then \
	  git lfs install --local --skip-repo >/dev/null && git lfs pull; \
	else \
	  echo "warning: git-lfs is not installed; the binary files (images, fonts, sounds) are LFS pointers until it is (brew install git-lfs, then make install-hooks)" >&2; \
	fi
	@echo "installed the hooks -> tools/git-hooks/ (banned-terms gate, a tag on every version bump, Git LFS)"

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
