ANDROID ?= 15
TAG ?= holtdroid-redroid:$(ANDROID)

.PHONY: image check
image:
	docker buildx build --platform linux/amd64 $(shell sed 's/^/--build-arg /' versions/$(ANDROID).env) -t $(TAG) $(ARGS) .

check:
	test -z "$$(gofmt -l .)" && GOOS=linux go vet ./... && python3 -m py_compile build/lpunpack.py
