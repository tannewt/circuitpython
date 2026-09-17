#!/usr/bin/env node

// SPDX-FileCopyrightText: 2014 MicroPython & CircuitPython contributors (https://github.com/adafruit/circuitpython/graphs/contributors)
//
// SPDX-License-Identifier: MIT

// Download every board's firmware size record from this run's artifacts, by exact name.
//
// build_release_files.py records a board's sizes in sizes/<board>.json and the board job
// uploads it as the artifact zz-sizes-<board>. actions/download-artifact matches a name
// pattern against a listing of the run's artifacts, and that listing stops at 1000, which
// a full build exceeds. Looking each record up by name has no such limit and uses the same
// internal artifact API, so it costs nothing against the REST rate limit.
//
// Usage: BOARDS='<the scheduler job's "ports" output>' node ci_download_sizes.mjs <out-dir>
// Needs @actions/artifact where node can resolve it: `npm install` at the repository root.

import { DefaultArtifactClient } from "@actions/artifact";

const outDir = process.argv[2] ?? "sizes";
const spec = JSON.parse(process.env.BOARDS || "{}");
const boards = (spec.ports ?? []).flatMap((port) => spec[port]);
const client = new DefaultArtifactClient();
const missing = [];
let downloaded = 0;

async function fetchRecord(board) {
  try {
    const { artifact } = await client.getArtifact(`zz-sizes-${board}`);
    await client.downloadArtifact(artifact.id, { path: outDir });
    downloaded++;
  } catch (error) {
    missing.push(board);
    console.log(`No size record for ${board}: ${error.message}`);
  }
}

// A few downloads at a time: enough to get through a full build in a minute or two,
// few enough not to trip the artifact service's throttling.
const queue = [...boards];
await Promise.all(
  Array.from({ length: 8 }, async () => {
    while (queue.length) {
      await fetchRecord(queue.shift());
    }
  }),
);

console.log(`Downloaded ${downloaded} of ${boards.length} size records to ${outDir}`);
if (missing.length) {
  console.log(`::warning::No firmware size record for ${missing.length} board(s): ${missing.join(" ")}`);
}
