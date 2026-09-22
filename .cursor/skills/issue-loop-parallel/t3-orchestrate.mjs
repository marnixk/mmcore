#!/usr/bin/env node
// Drive the local T3 Code orchestration HTTP API (spawn/stop worker threads).
//
// See SKILL.md ("Spawning workers" -> "Scripted (T3 host API)") for the why.
// The API is gated by "environment auth": a Bearer session token signed with
// the server's HMAC key. This script mints a short-lived session for each
// invocation and revokes it before exiting, so no credential is left behind.
//
//   node t3-orchestrate.mjs snapshot                 # projects + threads
//   node t3-orchestrate.mjs projects                 # project ids/paths only
//   node t3-orchestrate.mjs threads                  # id/status/title of threads
//   node t3-orchestrate.mjs spawn --slug <slug> --title <title> --prompt <text|@file>
//   node t3-orchestrate.mjs dispatch <command.json|->  # raw ClientOrchestrationCommand
//   node t3-orchestrate.mjs rm-thread <threadId>
//
// Requires node >= 18 (global fetch) and the `sqlite3` CLI on PATH.

import { execFileSync } from "node:child_process";
import { createHmac, randomUUID } from "node:crypto";
import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

const T3_HOME = join(homedir(), ".t3", "userdata");
const DB_PATH = join(T3_HOME, "state.sqlite");
const KEY_PATH = join(T3_HOME, "secrets", "server-signing-key.bin");
const SCOPES = [
  "orchestration:read",
  "orchestration:operate",
  "terminal:operate",
  "review:write",
  "access:read",
  "access:write",
  "relay:read",
];
const DEFAULT_MODEL_SELECTION = {
  instanceId: "opencode",
  model: "deepseek/deepseek-flash",
  options: [
    { id: "variant", value: "high" },
    { id: "agent", value: "build" },
  ],
};

function serverBase() {
  const rt = JSON.parse(readFileSync(join(T3_HOME, "server-runtime.json"), "utf8"));
  return `${rt.origin ?? `http://${rt.host}:${rt.port}`}`.replace(/\/$/, "");
}

function sqlite(sql) {
  return execFileSync("sqlite3", [DB_PATH], { input: sql, encoding: "utf8" });
}

function mintSession() {
  const secret = readFileSync(KEY_PATH);
  const sessionId = randomUUID();
  const now = Date.now();
  const claims = {
    v: 1,
    kind: "session",
    sid: sessionId,
    sub: "t3-orchestrate-script",
    scopes: SCOPES,
    method: "bearer-access-token",
    iat: now,
    exp: now + 5 * 60 * 1000,
  };
  const payload = Buffer.from(JSON.stringify(claims)).toString("base64url");
  const signature = createHmac("sha256", secret).update(payload).digest("base64url");
  const sc = JSON.stringify(SCOPES).replace(/'/g, "''");
  const iso = (ms) => new Date(ms).toISOString();
  sqlite(
    `INSERT INTO auth_sessions ` +
      `(session_id,subject,scopes,method,client_label,client_device_type,issued_at,expires_at,revoked_at) VALUES (` +
      `'${sessionId}','t3-orchestrate-script','${sc}','bearer-access-token','t3-orchestrate script','unknown',` +
      `'${iso(now)}','${iso(now + 5 * 60 * 1000)}',NULL);`,
  );
  return { token: `${payload}.${signature}`, sessionId };
}

function revokeSession(sessionId) {
  sqlite(
    `UPDATE auth_sessions SET revoked_at='${new Date().toISOString()}' WHERE session_id='${sessionId}';` +
      `DELETE FROM auth_sessions WHERE session_id='${sessionId}';`,
  );
}

async function withSession(fn) {
  const { token, sessionId } = mintSession();
  try {
    return await fn(token);
  } finally {
    try {
      revokeSession(sessionId);
    } catch (err) {
      process.stderr.write(`warn: failed to revoke session ${sessionId}: ${err}\n`);
    }
  }
}

async function api(token, path, body) {
  const res = await fetch(serverBase() + path, {
    method: body === undefined ? "GET" : "POST",
    headers: { authorization: `Bearer ${token}`, "content-type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await res.text();
  let parsed;
  try {
    parsed = JSON.parse(text);
  } catch {
    parsed = text;
  }
  if (!res.ok) throw new Error(`${res.status} ${path}: ${text}`);
  return parsed;
}

function findProject(snapshot, name) {
  const projects = snapshot.projects ?? [];
  if (!name) {
    if (projects.length === 1) return projects[0];
    throw new Error(`multiple projects; pass --project <name> (${projects.map((p) => p.title).join(", ")})`);
  }
  const match = projects.find(
    (p) =>
      p.title === name ||
      p.repositoryIdentity?.name === name ||
      p.repositoryIdentity?.displayName === name ||
      p.id === name,
  );
  if (!match) throw new Error(`no project matching '${name}'`);
  return match;
}

function isoNow() {
  return new Date().toISOString();
}

function command(kind, fields) {
  return { type: kind, commandId: randomUUID(), createdAt: isoNow(), ...fields };
}

const commands = {
  async snapshot() {
    await withSession(async (token) => {
      console.log(JSON.stringify(await api(token, "/api/orchestration/shell"), null, 2));
    });
  },

  async projects() {
    await withSession(async (token) => {
      const snap = await api(token, "/api/orchestration/shell");
      for (const p of snap.projects ?? []) {
        console.log(`${p.id}\t${p.title}\t${p.repositoryIdentity?.rootPath ?? p.workspaceRoot}`);
      }
    });
  },

  async threads() {
    await withSession(async (token) => {
      const snap = await api(token, "/api/orchestration/shell");
      for (const t of snap.threads ?? []) {
        console.log(`${t.id}\t${t.session?.status ?? "?"}\t${t.branch ?? "-"}\t${t.title}`);
      }
    });
  },

  async dispatch(argv) {
    const src = argv[0];
    if (!src) throw new Error("usage: dispatch <command.json|->");
    const text = src === "-" ? readFileSync(0, "utf8") : readFileSync(src, "utf8");
    const cmd = JSON.parse(text);
    await withSession(async (token) => {
      console.log(JSON.stringify(await api(token, "/api/orchestration/dispatch", cmd)));
    });
  },

  async "rm-thread"(argv) {
    const threadId = argv[0];
    if (!threadId) throw new Error("usage: rm-thread <threadId>");
    await withSession(async (token) => {
      console.log(JSON.stringify(await api(token, "/api/orchestration/dispatch", command("thread.delete", { threadId }))));
    });
  },

  async spawn(argv) {
    const flags = parseFlags(argv);
    const slug = required(flags, "slug");
    const title = flags.title ?? slug;
    const prompt = flags.prompt?.startsWith("@")
      ? readFileSync(flags.prompt.slice(1), "utf8")
      : required(flags, "prompt");
    const modelSelection = flags.model
      ? { instanceId: flags.instance ?? DEFAULT_MODEL_SELECTION.instanceId, model: flags.model, options: DEFAULT_MODEL_SELECTION.options }
      : DEFAULT_MODEL_SELECTION;
    const runtimeMode = flags.runtime ?? "full-access";
    const interactionMode = flags.interaction ?? "default";
    const baseBranch = flags.base ?? "master";

    await withSession(async (token) => {
      const snap = await api(token, "/api/orchestration/shell");
      const project = findProject(snap, flags.project);
      const projectCwd = project.repositoryIdentity?.rootPath ?? project.workspaceRoot;
      const branch = `task/${slug}-0ccd`;
      const threadId = randomUUID();
      const createdAt = isoNow();
      const cmd = command("thread.turn.start", {
        threadId,
        message: { messageId: randomUUID(), role: "user", text: prompt, attachments: [] },
        modelSelection,
        titleSeed: title,
        runtimeMode,
        interactionMode,
        bootstrap: {
          createThread: {
            projectId: project.id,
            title,
            modelSelection,
            runtimeMode,
            interactionMode,
            branch,
            worktreePath: null,
            createdAt,
          },
          prepareWorktree: {
            projectCwd,
            baseBranch,
            branch,
            startFromOrigin: true,
          },
        },
      });
      const result = await api(token, "/api/orchestration/dispatch", cmd);
      console.log(JSON.stringify({ threadId, branch, projectId: project.id, projectCwd, ...result }));
    });
  },
};

function required(flags, name) {
  const value = flags[name];
  if (value === undefined) throw new Error(`missing --${name}`);
  return value;
}

function parseFlags(argv) {
  const flags = {};
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg.startsWith("--")) {
      const [key, inline] = arg.slice(2).split("=", 2);
      flags[key] = inline !== undefined ? inline : argv[++i];
    }
  }
  return flags;
}

const [name, ...rest] = process.argv.slice(2);
const handler = commands[name];
if (!handler) {
  console.error(`usage: t3-orchestrate.mjs <${Object.keys(commands).join("|")}>`);
  process.exit(2);
}
handler(rest).catch((err) => {
  console.error(err.message ?? err);
  process.exit(1);
});
