FROM golang:1.26.9-bookworm AS build
WORKDIR /src
ENV GOTOOLCHAIN=local
COPY go.mod go.sum ./
RUN go mod download && go mod verify
COPY cmd ./cmd
COPY internal ./internal
COPY migrations ./migrations
RUN test -z "$(gofmt -l cmd internal migrations)" && go vet ./... && go test -race ./...
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/box3-ota ./cmd/box3-ota

FROM scratch
COPY --from=build /out/box3-ota /box3-ota
USER 10001:10001
STOPSIGNAL SIGTERM
ENTRYPOINT ["/box3-ota"]
CMD ["--config", "/etc/box3-ota/config.json", "--require-loopback"]
