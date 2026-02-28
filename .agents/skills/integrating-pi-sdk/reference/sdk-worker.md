# SDK Worker: Headless fbmq + Pi

Full Node.js implementation for a headless worker that polls an fbmq queue and processes each task through a Pi agent session.

## Source: `worker.ts`

```typescript
import {
  createAgentSession,
  AuthStorage,
  ModelRegistry,
  SessionManager,
  SettingsManager,
} from "@mariozechner/pi-coding-agent";
import { execSync } from "node:child_process";

const QUEUE = "/var/queue/code-tasks";

async function processTask(body: string): Promise<boolean> {
  const authStorage = AuthStorage.create();
  const { session } = await createAgentSession({
    sessionManager: SessionManager.inMemory(),
    settingsManager: SettingsManager.create(),
    authStorage,
    modelRegistry: new ModelRegistry(authStorage),
  });

  // Subscribe to output
  session.subscribe((event) => {
    if (event.type === "message_update" &&
        event.assistantMessageEvent.type === "text_delta") {
      process.stdout.write(event.assistantMessageEvent.delta);
    }
  });

  try {
    await session.prompt(body);
    return true;
  } catch (err) {
    console.error("Agent failed:", err);
    return false;
  }
}

async function main() {
  console.log("Worker started — polling queue...");

  while (true) {
    // Check depth
    const depth = parseInt(
      execSync(`fbmq depth ${QUEUE}`, { encoding: "utf-8" }).trim(), 10
    );

    if (depth === 0) {
      await new Promise((r) => setTimeout(r, 5000)); // poll every 5s
      continue;
    }

    // Pop next task
    const taskPath = execSync(`fbmq pop ${QUEUE}`, { encoding: "utf-8" }).trim();
    const body = execSync(`fbmq cat "${taskPath}"`, { encoding: "utf-8" });
    console.log(`\n--- Processing: ${taskPath} ---\n${body}\n`);

    // Run through Pi SDK
    const success = await processTask(body);

    // Ack or nack
    if (success) {
      execSync(`fbmq ack ${QUEUE} "${taskPath}"`);
      console.log("✓ Task completed");
    } else {
      execSync(`fbmq nack ${QUEUE} "${taskPath}"`);
      console.log("✗ Task failed → dead-letter");
    }
  }
}

main();
```

## Running the Worker

### With pm2
```bash
npx pm2 start worker.ts --name fbmq-worker --interpreter tsx
```

### With systemd
```ini
[Unit]
Description=fbmq Pi Worker
After=network.target

[Service]
ExecStart=/usr/bin/npx tsx /opt/workers/worker.ts
Restart=always
Environment=QUEUE=/var/queue/code-tasks

[Install]
WantedBy=multi-user.target
```

## Scaling to Multiple Agents

Run multiple workers on different queues for parallel multi-agent pipelines:

```bash
QUEUE=/var/queue/frontend-tasks npx tsx worker.ts &
QUEUE=/var/queue/backend-tasks  npx tsx worker.ts &
QUEUE=/var/queue/test-tasks     npx tsx worker.ts &
```

A coordinator process pushes correlated tasks (using `fbmq push -C <correlation-id>`) across all queues. Each worker independently pops, processes, and acks.
