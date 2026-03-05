# The fbmq Manifesto

## The filesystem is the queue

Operating systems already ship a battle-tested, POSIX-standardized,
crash-recoverable storage layer with atomic operations, hierarchical
namespaces, and permission controls. It is called the filesystem.

fbmq does not sit on top of this infrastructure. It *is* this
infrastructure. A queue is a directory. A message is a file. A claim is a
`rename(2)`. There is no broker, no daemon, no protocol, no port, no
dependency. There is a directory and a 100 KB static binary.

---

## Principles

### The filesystem is enough

`rename(2)` is atomic on every POSIX system. Directories are namespaces.
Files are messages. Permissions are access control. `inotifywait` is your
event loop. The kernel already solved coordination — don't reinvent it
with application-level locking, leader election, or consensus protocols
when a single rename does the job.

### Messages are files, queues are directories

Every message is a Markdown file with RFC 822 headers. You can `cat` it,
`grep` it, `diff` it, email it, version-control it. Your queue is
visible with `ls`. Your dead-letter queue is a directory you can browse.
No opaque binary formats. No serialization libraries. No special tooling
required to see what is happening.

### Zero dependencies, zero daemons

fbmq is a single C binary compiled against libc. No runtime to install.
No server process to monitor. No ports to open. No configuration files
to maintain. It builds with `make` and runs on any POSIX system with a
filesystem. That's the dependency list.

### Simplicity over cleverness

A flat directory is the right data structure until measurements prove
otherwise. fbmq does not shard, partition, or pre-allocate. `pending/`
is a single directory. When you need priority levels, it becomes four
directories — one per level. That is the entire topology. Cleverness
has a maintenance cost that almost always exceeds the performance gain
it was designed to provide. Start simple, measure, and only then add
complexity — and if the measurements never justify it, neither does the
code.

### No FIFO guarantee

fbmq does not guarantee message ordering. `pop` grabs the first
available entry from `readdir` — the order is filesystem-dependent and
varies across ext4, XFS, APFS, and tmpfs. Sorting entries for strict
FIFO would cost O(n) per pop, a price most workloads never need to pay.
Under multi-consumer contention, ordering is already non-deterministic:
multiple consumers race on `rename(2)`, so who gets which message
depends on scheduling. If your workflow requires ordered processing,
enforce it at the application layer — sequence numbers in headers,
correlation IDs, or explicit dependencies between messages.

### Explicit over automatic

There are no background threads. No hidden garbage collection. No
surprise compaction. You call `reap` when you want to reclaim stale
messages. You call `purge` when you want to clean old completions. You
choose whether to fsync per message or skip it entirely with
`--no-fsync`. You set the maximum queue size at init time — the default
is 10,000 pending messages, but `--max-pending 0` removes the limit
entirely. Every operational decision is yours to make, not the queue's.

### One tool, one output

`pop` prints a path. That's it. Read the message yourself with `cat`.
Parse it with `awk`. Route it with a shell script. fbmq produces one
piece of information per command and gets out of the way. Compose it with
Unix pipes — don't wait for it to grow a plugin system.

### Fail-safe by default

Per-message `fsync`. Lease-based crash recovery. Automatic dead-lettering
after configurable retries. TTL expiry for time-bounded work. The safe
path is the default path. If you want speed over durability — tmpfs and
`--no-fsync` will get you 100K+ messages per second — but you have to
ask for it.

---

## When fbmq fits

**Single-host job queues.** A web server drops work into `pending/`. A
pool of cron workers each call `pop`, process the message, and `ack`. No
broker to provision. No client library to import.

**Development and testing.** Spin up a queue in a temp directory. Push
messages. Inspect them with `ls` and `cat`. No containers to launch, no
ports to bind, no teardown scripts. Delete the directory when you're
done.

**Edge, IoT, and embedded systems.** A static binary under 100 KB with
no runtime dependencies. Cross-compile it, drop it on the device, point
it at a directory. It works on any POSIX system with a filesystem.

**CI/CD pipelines.** Coordinate build stages through the filesystem.
Push artifacts as messages. Workers pop and process. The queue state is
visible to every tool that can read a directory.

**Audit trails.** Messages are durable Markdown files with timestamps,
correlation IDs, retry counts, and tags baked into RFC 822 headers. They
don't vanish after consumption — `done/` is your receipt archive.

**AI agent task queues.** Give an agent harness a work directory instead
of a message broker. The agent pushes sub-tasks as Markdown files —
human-readable, auditable, with priority and correlation IDs built in.
Orchestrators `pop` tasks, dispatch to models or tool-calling loops, and
`ack` or `nack` based on results. Dead-lettering catches failed
generations. The entire agent state is `ls` and `cat` — no dashboards,
no observability stack, just files you can read. Swap models, retry
manually, inspect chain-of-thought artifacts, all with Unix tools.

**Prototyping distributed patterns.** Work queues, task fans, priority
scheduling, dead-letter routing, lease-based recovery — all on a single
machine, all inspectable, before you commit to a distributed broker.

---

## When fbmq does not fit

**Multi-node replication.** fbmq operates on a single filesystem. If you
need messages replicated across machines, you need a distributed broker.

**Sub-microsecond latency.** Filesystem operations have kernel overhead.
If you need nanosecond-scale message passing, use shared memory or a
lock-free ring buffer.

**Pub/sub and fan-out.** fbmq is a work queue: one message, one
consumer. If you need topic-based routing or multiple subscribers per
message, look elsewhere.

**You already have a broker.** If RabbitMQ or Redis is already in your
stack and meeting your needs, fbmq is not asking you to rip it out. Use
it where a full broker is overhead you don't need.

---

## Closing

Most queuing problems are simpler than the solutions deployed against
them. fbmq exists because a directory of files, moved atomically, is
often all you need — and recognizing that is not a limitation. It is the
design.
