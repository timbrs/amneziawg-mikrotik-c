VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
IMAGE_NAME = awg-proxy
BUILD_DIR = ../builds

SRCS = src/main.c src/proxy.c src/transform.c src/blake2s.c src/chacha20.c src/cps.c src/fastrand.c src/base64.c src/log.c
CFLAGS = -O2 -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -ffunction-sections -fdata-sections -flto -DVERSION=\"$(VERSION)\"
LDFLAGS = -static -Wl,--gc-sections -flto -s -lpthread

.PHONY: build clean test test-blake2s test-chacha20 test-cps test-transform test-base64 test-session test-dns test-stress \
	docker-arm64 docker-arm docker-armv5 docker-amd64 docker-all \
	docker-arm64-7.20-docker docker-arm-7.20-docker docker-armv5-7.20-docker docker-amd64-7.20-docker docker-all-7.20-docker

TEST_SRCS = src/blake2s.c src/chacha20.c src/cps.c src/transform.c src/fastrand.c src/base64.c src/log.c
TEST_CFLAGS = -g -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE

build:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(IMAGE_NAME) $(SRCS)

test: test-blake2s test-chacha20 test-cps test-transform test-base64 test-session test-dns
	@echo "All tests passed"

test-blake2s: src/test_blake2s.c $(TEST_SRCS)
	$(CC) $(TEST_CFLAGS) -o /tmp/test_blake2s $^
	/tmp/test_blake2s

test-chacha20: src/test_chacha20.c $(TEST_SRCS)
	$(CC) $(TEST_CFLAGS) -o /tmp/test_chacha20 $^
	/tmp/test_chacha20

test-cps: src/test_cps.c $(TEST_SRCS)
	$(CC) $(TEST_CFLAGS) -o /tmp/test_cps $^
	/tmp/test_cps

test-transform: src/test_transform.c $(TEST_SRCS)
	$(CC) $(TEST_CFLAGS) -o /tmp/test_transform $^
	/tmp/test_transform

test-base64: src/test_base64.c $(TEST_SRCS)
	$(CC) $(TEST_CFLAGS) -o /tmp/test_base64 $^
	/tmp/test_base64

test-session: src/test_session.c $(TEST_SRCS)
	$(CC) $(TEST_CFLAGS) -o /tmp/test_session $^
	/tmp/test_session

test-dns: src/test_dns.c src/proxy.c $(TEST_SRCS)
	$(CC) $(TEST_CFLAGS) -o /tmp/test_dns $^ -lpthread
	/tmp/test_dns

# Stress test — manual only, NOT part of `make test` or CI
test-stress: src/test_stress.c build
	$(CC) $(TEST_CFLAGS) -lpthread -o /tmp/test_stress src/test_stress.c
	/tmp/test_stress

clean:
	rm -f $(BUILD_DIR)/$(IMAGE_NAME) $(BUILD_DIR)/$(IMAGE_NAME)-*

# --- OCI Docker images via buildx ---
docker-arm64:
	@mkdir -p $(BUILD_DIR)
	docker buildx build --platform linux/arm64 \
		--build-arg VERSION=$(VERSION) \
		--output type=oci,dest=$(BUILD_DIR)/$(IMAGE_NAME)-arm64.tar \
		-t $(IMAGE_NAME):$(VERSION)-arm64 .
	gzip -f $(BUILD_DIR)/$(IMAGE_NAME)-arm64.tar

docker-arm:
	@mkdir -p $(BUILD_DIR)
	docker buildx build --platform linux/arm/v7 \
		--build-arg VERSION=$(VERSION) \
		--output type=oci,dest=$(BUILD_DIR)/$(IMAGE_NAME)-arm.tar \
		-t $(IMAGE_NAME):$(VERSION)-arm .
	gzip -f $(BUILD_DIR)/$(IMAGE_NAME)-arm.tar

docker-armv5:
	@mkdir -p $(BUILD_DIR)
	docker buildx build --platform linux/arm/v5 \
		--build-arg VERSION=$(VERSION) \
		--output type=oci,dest=$(BUILD_DIR)/$(IMAGE_NAME)-armv5.tar \
		-t $(IMAGE_NAME):$(VERSION)-armv5 .
	gzip -f $(BUILD_DIR)/$(IMAGE_NAME)-armv5.tar

docker-amd64:
	@mkdir -p $(BUILD_DIR)
	docker buildx build --platform linux/amd64 \
		--build-arg VERSION=$(VERSION) \
		--output type=oci,dest=$(BUILD_DIR)/$(IMAGE_NAME)-amd64.tar \
		-t $(IMAGE_NAME):$(VERSION)-amd64 .
	gzip -f $(BUILD_DIR)/$(IMAGE_NAME)-amd64.tar

docker-all: docker-arm64 docker-arm docker-armv5 docker-amd64

# --- Classic Docker tar (RouterOS 7.20 LT) ---
docker-arm64-7.20-docker:
	@mkdir -p $(BUILD_DIR)
	VERSION=$(VERSION) ../scripts/mkdockertar-c.sh linux arm64 "" $(IMAGE_NAME):$(VERSION)-arm64 $(BUILD_DIR)/$(IMAGE_NAME)-arm64-7.20-Docker.tar.gz

docker-arm-7.20-docker:
	@mkdir -p $(BUILD_DIR)
	VERSION=$(VERSION) ../scripts/mkdockertar-c.sh linux arm 7 $(IMAGE_NAME):$(VERSION)-arm $(BUILD_DIR)/$(IMAGE_NAME)-arm-7.20-Docker.tar.gz

docker-armv5-7.20-docker:
	@mkdir -p $(BUILD_DIR)
	VERSION=$(VERSION) ../scripts/mkdockertar-c.sh linux arm 5 $(IMAGE_NAME):$(VERSION)-armv5 $(BUILD_DIR)/$(IMAGE_NAME)-armv5-7.20-Docker.tar.gz

docker-amd64-7.20-docker:
	@mkdir -p $(BUILD_DIR)
	VERSION=$(VERSION) ../scripts/mkdockertar-c.sh linux amd64 "" $(IMAGE_NAME):$(VERSION)-amd64 $(BUILD_DIR)/$(IMAGE_NAME)-amd64-7.20-Docker.tar.gz

docker-all-7.20-docker: docker-arm64-7.20-docker docker-arm-7.20-docker docker-armv5-7.20-docker docker-amd64-7.20-docker
