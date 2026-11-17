# stephanos_dqueue

A lightweight distributed message queue (`dqueue`) written from scratch in **C++20** using **POSIX sockets** and **Bazel** — with zero external libraries.

## Features

- **Thread-safe named queues** (`MessageQueue`, `QueueManager`) with blocking `POP` timeouts
- **At-least-once delivery** via `ACK`, `NACK`, and automatic visibility-timeout redelivery
- **Line-based TCP protocol** (`PUSH`, `POP`, `ACK`, `NACK`, `SIZE`, `LIST`, `DELETE`, `PING`)
- **Multi-threaded broker server** backed by a fixed `ThreadPool` and `poll()` accept loop
- **Write-Ahead Log (WAL)** with `fsync`, crash recovery, and snapshot compaction
- **Leader → Follower replication** (`REPL_PUSH`, `REPL_ACK`)
- **Cluster partitioning** via consistent hashing with virtual nodes (`ConsistentHashRing`, `PartitionedClient`)
- **CLI tool (`dq_cli`)** for single-broker and partitioned cluster operations

## Project Structure

```
.
├── BUILD.bazel                  # Root aliases (//:dqueue, //:dq_cli)
├── MODULE.bazel                 # Bazel module & rules_cc dependency
├── .bazelrc                     # C++20, tsan config, build/ symlink prefix
└── src/
    ├── main.cpp                 # dqueue broker entrypoint
    ├── cli/                     # dq_cli producer/consumer tool
    ├── client/                  # TCP Client & PartitionedClient
    ├── message_queue/           # Thread-safe queue with leases & redelivery
    ├── partition/               # ConsistentHashRing (FNV-1a + vnodes)
    ├── protocol/                # Wire protocol parser & serializer
    ├── queue_manager/           # Named queue registry
    ├── replication/             # Async leader -> follower replicator
    ├── server/                  # POSIX TCP broker server
    ├── thread_pool/             # Worker thread pool
    └── wal/                     # Write-ahead log & crash recovery
```

## Build & Test

```sh
# Build broker, CLI, and all libraries
bazel build //...

# Run all unit & integration tests
bazel test //...

# Run all tests under ThreadSanitizer
bazel test --config=tsan //...
```

All Bazel output symlinks (`bin`, `out`, `testlogs`) are placed in `build/`.

## Quick Start

Start a broker with WAL persistence:

```sh
bazel run //:dqueue -- --port 8080 --threads 4 --visibility-ms 5000 --wal /tmp/dqueue.wal
```

Push, pop, and ack messages using `dq_cli`:

```sh
bazel run //:dq_cli -- push orders '{"id":1}'
bazel run //:dq_cli -- pop orders 1000
bazel run //:dq_cli -- ack orders 1
bazel run //:dq_cli -- list
```

### Leader → Follower Replication

```sh
# Start follower
bazel run //:dqueue -- --port 8081 --wal /tmp/follower.wal

# Start leader pointing at follower
bazel run //:dqueue -- --port 8080 --wal /tmp/leader.wal --followers 127.0.0.1:8081
```

### Partitioned Multi-Broker Routing

```sh
bazel run //:dq_cli -- --brokers 127.0.0.1:8080,127.0.0.1:8081,127.0.0.1:8082 push orders '{"id":42}'
bazel run //:dq_cli -- --brokers 127.0.0.1:8080,127.0.0.1:8081,127.0.0.1:8082 pop orders
```
