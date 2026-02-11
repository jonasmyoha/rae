.PHONY: all dev build test stop gemini

all: dev

dev:
	@$(MAKE) -C rae-devtools-web dev

build:
	@$(MAKE) -C rae/compiler build

test:
	@$(MAKE) -C rae/compiler test

stop:
	@$(MAKE) -C rae-devtools-web stop

gemini:
	@exec gemini --yolo --resume latest
