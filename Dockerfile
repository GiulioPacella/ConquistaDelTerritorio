
# compilazione

    FROM debian:bookworm-slim AS build

RUN apt-get update && \
    apt-get install -y --no-install-recommends build-essential && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN make

# runtime del client 

FROM debian:bookworm-slim AS client

COPY --from=build /src/client /app/client

ENTRYPOINT ["/app/client"]
CMD ["server", "8080"]

# runtime del server 

FROM debian:bookworm-slim AS server

COPY --from=build /src/server /app/server

WORKDIR /data

EXPOSE 8080

ENTRYPOINT ["/app/server"]
CMD ["8080"]
