# Pi Extension: fbmq Integration

Full TypeScript source for `.pi/extensions/fbmq.ts`. This registers queue tools and slash commands with the Pi agent.

## Source

```typescript
import type { ExtensionAPI } from "@mariozechner/pi-coding-agent";
import { Type } from "@sinclair/typebox";
import { execSync } from "node:child_process";

const QUEUE_DIR = process.env.FBMQ_QUEUE || "/tmp/tasks";

export default function (pi: ExtensionAPI) {
  // --- Tool: push a task to the queue ---
  pi.registerTool({
    name: "queue_push",
    label: "Queue Push",
    description: "Push a task to the fbmq work queue",
    parameters: Type.Object({
      body: Type.String({ description: "Task description (Markdown)" }),
      priority: Type.Optional(
        Type.Union([
          Type.Literal("critical"),
          Type.Literal("high"),
          Type.Literal("normal"),
          Type.Literal("low"),
        ])
      ),
      tags: Type.Optional(Type.String({ description: "Comma-separated tags" })),
      correlationId: Type.Optional(Type.String()),
    }),
    async execute({ body, priority, tags, correlationId }, ctx) {
      let cmd = `echo ${JSON.stringify(body)} | fbmq push ${QUEUE_DIR}`;
      if (priority) cmd += ` -p ${priority}`;
      if (tags) cmd += ` -T ${tags}`;
      if (correlationId) cmd += ` -C ${correlationId}`;

      const id = execSync(cmd, { encoding: "utf-8" }).trim();
      ctx.ui.notify(`Queued: ${id}`, "success");
      return { id };
    },
  });

  // --- Tool: pop and read the next task ---
  pi.registerTool({
    name: "queue_pop",
    label: "Queue Pop",
    description: "Claim the next task from the queue and return its body",
    parameters: Type.Object({}),
    async execute(_, ctx) {
      try {
        const path = execSync(`fbmq pop ${QUEUE_DIR}`, { encoding: "utf-8" }).trim();
        const body = execSync(`fbmq cat "${path}"`, { encoding: "utf-8" });
        const metadata = execSync(`fbmq inspect "${path}"`, { encoding: "utf-8" });
        return { path, body, metadata };
      } catch {
        return { error: "Queue empty" };
      }
    },
  });

  // --- Tool: ack/nack ---
  pi.registerTool({
    name: "queue_complete",
    label: "Queue Complete",
    description: "Mark a task as done (ack) or failed (nack)",
    parameters: Type.Object({
      path: Type.String({ description: "Task file path from queue_pop" }),
      success: Type.Boolean({ description: "true to ack, false to nack" }),
    }),
    async execute({ path, success }, ctx) {
      const cmd = success
        ? `fbmq ack ${QUEUE_DIR} "${path}"`
        : `fbmq nack ${QUEUE_DIR} "${path}"`;
      execSync(cmd);
      ctx.ui.notify(success ? "Task completed" : "Task failed → dead-letter",
                     success ? "success" : "warn");
      return { status: success ? "done" : "failed" };
    },
  });

  // --- Tool: queue depth ---
  pi.registerTool({
    name: "queue_depth",
    label: "Queue Depth",
    description: "Check how many tasks are pending",
    parameters: Type.Object({}),
    async execute() {
      const depth = execSync(`fbmq depth ${QUEUE_DIR}`, { encoding: "utf-8" }).trim();
      return { pending: parseInt(depth, 10) };
    },
  });

  // --- Command: /work — process all queued tasks ---
  pi.registerCommand("work", {
    description: "Pop and process all tasks from the queue",
    handler: async (_args, ctx) => {
      const depth = execSync(`fbmq depth ${QUEUE_DIR}`, { encoding: "utf-8" }).trim();
      ctx.ui.notify(`${depth} tasks pending — starting work loop`, "info");
      pi.sendUserMessage(
        `There are ${depth} pending tasks in the queue at ${QUEUE_DIR}. ` +
        `Pop each one using queue_pop, do the work described in the body, ` +
        `then ack with queue_complete. Continue until the queue is empty.`
      );
    },
  });

  // --- Command: /plan — decompose work into queued tasks ---
  pi.registerCommand("plan", {
    description: "Analyze the project and queue tasks",
    handler: async (args, ctx) => {
      pi.sendUserMessage(
        `Analyze this project and create a plan for: ${args || "general improvements"}. ` +
        `For each task, use queue_push with an appropriate priority and descriptive body. ` +
        `Tag related tasks with a shared correlation ID.`
      );
    },
  });
}
```

## Environment Variables

| Variable | Default | Description |
|---|---|---|
| `FBMQ_QUEUE` | `/tmp/tasks` | Queue directory path |

## Registered Tools

| Tool | Description |
|---|---|
| `queue_push` | Push a task with body, priority, tags, correlation ID |
| `queue_pop` | Claim next task, returns path + body + metadata |
| `queue_complete` | Ack or nack a claimed task |
| `queue_depth` | Return count of pending messages |

## Slash Commands

| Command | Description |
|---|---|
| `/plan <goal>` | Pi analyzes the project and queues decomposed tasks |
| `/work` | Pi processes all queued tasks in a pop→do→ack loop |
