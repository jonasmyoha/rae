.PHONY: all dev stop

DEV_CMD=bun run scripts/dev.ts

all: dev

dev:
	@echo "Starting Rae devtools server..."
	$(DEV_CMD)

stop:
	@bun run scripts/stop.ts
