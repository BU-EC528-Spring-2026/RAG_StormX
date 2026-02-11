# Variables
COMPOSE=docker-compose
DEV_SERVICE=dev

.PHONY: up down shell build-lib test-fresh clean

# 1. Start the environment
up:
	$(COMPOSE) up -d

# 2. Drop into the shell (Replaces your long docker run command)
shell:
	$(COMPOSE) exec $(DEV_SERVICE) /bin/bash

# 3. Stop the environment
down:
	$(COMPOSE) down

# 4. A "Shortcut" to build the library inside the container
build-lib:
	$(COMPOSE) exec $(DEV_SERVICE) bash -c "mkdir -p SPTAG/build && cd SPTAG/build && cmake .. && make -j$$(nproc)"

# 5. Run your freshness test
test:
	$(COMPOSE) exec $(DEV_SERVICE) python3 scripts/test_freshness.py

# 6. Wipe the slate clean
clean:
	rm -rf SPTAG/build
	$(COMPOSE) down --rmi local
