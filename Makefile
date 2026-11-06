# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -D_GNU_SOURCE -pthread
override LDLIBS += -lgnutls -lm -pthread

SRCS := src/main.c src/bridge.c src/cast.c src/cast_proto.c src/json.c src/mdns.c src/stream.c src/log.c
OBJS := $(SRCS:.c=.o)
LIB_SRCS := $(filter-out src/main.c,$(SRCS))
TESTS := tests/test_unit

all: airplay2cast

airplay2cast: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_unit: tests/test_unit.c $(LIB_SRCS) src/*.h
	$(CC) $(CFLAGS) -Isrc -fsanitize=address,undefined -o $@ tests/test_unit.c $(LIB_SRCS) $(LDLIBS)

test: $(TESTS)
	./tests/test_unit

install: airplay2cast
	install -D -m 755 airplay2cast $(DESTDIR)/usr/local/bin/airplay2cast

clean:
	rm -f airplay2cast $(OBJS) $(TESTS)

# --- Docker ------------------------------------------------------------------
# All build steps run on this machine; PLATFORM cross-compiles (no emulation).
#   make docker-image PLATFORM=linux/arm64
#   make docker-deploy DEPLOY_HOST=user@pine64
#   make docker-test DEPLOY_HOST=user@pine64 [TEST_SPEAKER="Hallway speaker"]
PLATFORM     ?=
IMAGE        ?= airplay2cast:latest
OUT          ?= out
DEPLOY_HOST  ?=
DEPLOY_DIR   ?= airplay2cast
TEST_SPEAKER ?=
DOCKER_BUILD := docker buildx build $(if $(PLATFORM),--platform $(PLATFORM))

docker-image:
	$(DOCKER_BUILD) -t $(IMAGE) --load .

# programs, license texts, sources and tests/test_unit for running on the target
docker-binaries:
	$(DOCKER_BUILD) --target binaries --output type=local,dest=$(OUT) .

check-deploy-host:
	@test -n "$(DEPLOY_HOST)" || { echo "set DEPLOY_HOST=user@host" >&2; exit 1; }

# copies the image and compose.yaml to DEPLOY_HOST and (re)starts the service there
docker-deploy: check-deploy-host
	docker save $(IMAGE) | gzip -1 | ssh $(DEPLOY_HOST) 'gunzip | docker load'
	ssh $(DEPLOY_HOST) 'mkdir -p $(DEPLOY_DIR)'
	scp -q compose.yaml $(DEPLOY_HOST):$(DEPLOY_DIR)/compose.yaml
	ssh $(DEPLOY_HOST) 'cd $(DEPLOY_DIR) && docker compose up -d --no-build --force-recreate'

# on DEPLOY_HOST: unit tests (from docker-binaries), receivers of the deployed image,
# and with TEST_SPEAKER the native integration test against that Cast device
docker-test: check-deploy-host
	rsync -a --delete $(OUT)/ $(DEPLOY_HOST):$(DEPLOY_DIR)/out/
	rsync -a tests scripts $(DEPLOY_HOST):$(DEPLOY_DIR)/
	ssh $(DEPLOY_HOST) 'cd $(DEPLOY_DIR) && out/tests/test_unit'
	ssh $(DEPLOY_HOST) 'since=$$(docker inspect -f "{{.State.StartedAt}}" airplay2cast) || exit 1; \
	  count() { docker logs --since "$$since" airplay2cast 2>&1 | grep -c "AirPlay 2 receiver .* started"; }; \
	  for i in $$(seq 60); do [ "$$(count)" -gt 0 ] && break; sleep 0.5; done; sleep 3; n=$$(count); \
	  docker ps --filter name=^airplay2cast$$ --format "{{.Status}}" | grep -q ^Up && [ "$$n" -gt 0 ] \
	  && echo "airplay2cast running, $$n AirPlay 2 receivers started"'
	$(if $(TEST_SPEAKER),ssh $(DEPLOY_HOST) 'cd $(DEPLOY_DIR) && tests/integration.sh "$(TEST_SPEAKER)" out/airplay2cast')

.PHONY: all test install clean docker-image docker-binaries docker-deploy docker-test check-deploy-host
