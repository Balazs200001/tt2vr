#!/usr/bin/env node
// gq.js - client for TT2QueryServer.java (file-based Ghidra query server).
//
// Usage:
//   node gq.js "decompile 0x1404dc530" "callers 0x1404dc530"
//   node gq.js --file queries.txt          (one command per line; # comments allowed)
// Options:
//   --work <dir>      server work dir (default: $TT2VR_GHIDRA_WORK or C:/dev/tt2vr-re/ghidra-query)
//   --timeout <sec>   per-batch timeout (default 900)
//   --out <file>      also write the combined output to this file
// Commands are listed in TT2QueryServer.java. A target written as an exact mangled export name ("?Foo@Bar@@...")
// is resolved to its VA through the exports TSV first, which is more reliable than symbol lookup.
'use strict';
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
let work = process.env.TT2VR_GHIDRA_WORK || 'C:/dev/tt2vr-re/ghidra-query';
let timeoutSec = 900;
let outFile = null;
const cmds = [];
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--work') work = args[++i];
  else if (a === '--timeout') timeoutSec = Number(args[++i]);
  else if (a === '--out') outFile = args[++i];
  else if (a === '--file') {
    for (const line of fs.readFileSync(args[++i], 'utf8').split(/\r?\n/)) {
      const t = line.trim();
      if (t && !t.startsWith('#')) cmds.push(t);
    }
  } else cmds.push(a);
}
if (!cmds.length) {
  console.error('usage: node gq.js "<command>" [...] | --file queries.txt');
  process.exit(1);
}
const inbox = path.join(work, 'inbox');
const outbox = path.join(work, 'outbox');
if (!fs.existsSync(path.join(work, 'ready'))) {
  console.error(`query server not running (no ${path.join(work, 'ready')}); start tools/ghidra/start-query-server.ps1`);
  process.exit(2);
}

// Resolve exact mangled names via the exports TSV (column 1 = VA, column 4 = mangled).
let exportsMap = null;
function resolveMangled(cmd) {
  const m = cmd.match(/^(\S+)\s+(\?\S+)(.*)$/);
  if (!m) return cmd;
  if (!exportsMap) {
    exportsMap = new Map();
    const tsv = process.env.TT2VR_EXPORTS_TSV || 'C:/dev/tt2vr-re/dumps/exports_4989319.tsv';
    if (fs.existsSync(tsv)) {
      for (const line of fs.readFileSync(tsv, 'utf8').split('\n')) {
        const c = line.split('\t');
        if (c.length >= 4) exportsMap.set(c[3], c[0]);
      }
    }
  }
  const va = exportsMap.get(m[2]);
  return va ? `${m[1]} ${va}${m[3]}` : cmd;
}

const stamp = `${Date.now().toString(36)}-${process.pid}`;
const pending = cmds.map((c, i) => {
  const id = `${stamp}-${String(i).padStart(4, '0')}`;
  const sent = resolveMangled(c);
  fs.writeFileSync(path.join(inbox, id + '.tmp'), sent, 'utf8');
  fs.renameSync(path.join(inbox, id + '.tmp'), path.join(inbox, id + '.q'));
  return { id, cmd: c, sent };
});

const deadline = Date.now() + timeoutSec * 1000;
const results = new Map();
function poll() {
  for (const p of pending) {
    if (results.has(p.id)) continue;
    const f = path.join(outbox, p.id + '.out');
    if (fs.existsSync(f)) {
      try {
        results.set(p.id, fs.readFileSync(f, 'utf8'));
        fs.unlinkSync(f);
      } catch (e) {
        // file may still be in the middle of the atomic rename; retry next tick
      }
    }
  }
  if (results.size === pending.length || Date.now() > deadline) return finish();
  setTimeout(poll, 150);
}
function finish() {
  const parts = [];
  for (const p of pending) {
    const header = `=== ${p.cmd}${p.sent !== p.cmd ? `   [sent: ${p.sent}]` : ''}`;
    parts.push(header + '\n' + (results.has(p.id) ? results.get(p.id) : `ERROR timeout after ${timeoutSec}s (query left in inbox)`));
  }
  const text = parts.join('\n\n') + '\n';
  if (outFile) fs.writeFileSync(outFile, text, 'utf8');
  process.stdout.write(text);
  process.exit(results.size === pending.length ? 0 : 3);
}
poll();
