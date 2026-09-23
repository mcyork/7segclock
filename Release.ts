#!/usr/bin/env bun
/*
 * Release.ts — build, verify and publish a 7segclock firmware release.
 *
 * THE FROZEN CONTRACT
 * Devices in the field run the 1.0.0 updater, and it cannot be changed from
 * here. It looks for the latest release of the public repo mcyork/7segclock
 * and expects:
 *   - a release that is not a draft, not a prerelease, and is marked Latest;
 *   - a tag of the form vX.Y.Z;
 *   - an asset named exactly `firmware.bin` that is an APP image (not the
 *     merged factory image) and at most 0x1E0000 bytes (one OTA app slot in
 *     min_spiffs.csv).
 * Anything that breaks one of those strands every fielded clock on the
 * version it is running. This script refuses rather than bend any of them.
 *
 * WHY TWO IMAGES GO TO TWO PLACES
 * `pio run` produces two binaries. `firmware.bin` is the application image:
 * the thing the device writes into its spare OTA slot. `firmware.factory.bin`
 * is the merged image — bootloader and partition table at offset 0, the app at
 * 0x10000 — for flashing a blank board.
 *   - The browser installer (ESP Web Tools, served by GitHub Pages from docs/)
 *     downloads its image with fetch(). GitHub release assets are served
 *     without an `access-control-allow-origin` header, so a browser page on
 *     another origin cannot read them. The installer's copy of the factory
 *     image therefore has to live in docs/, next to manifest.json.
 *   - The device is not a browser and has no CORS, so it pulls `firmware.bin`
 *     straight from the Release.
 *   - The factory image must never be the Release's `firmware.bin`: it starts
 *     with the bootloader, and the updater would write that into an app slot.
 *
 * USAGE
 *   bun Release.ts [--check]          read-only preflight (the default)
 *   bun Release.ts --build            pio run, verify both images, stage docs/
 *   bun Release.ts --publish          tag, push, create the GitHub Release
 *   add --dry-run to any mode         print mutations instead of doing them
 *
 * FLOW
 *   bun Release.ts --check            → bump FW_VERSION in src/main.cpp
 *   bun Release.ts --build            → review, then git add docs && git commit
 *   bun Release.ts --publish
 */

import { copyFileSync, existsSync, mkdtempSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";

const REPO_ROOT: string = import.meta.dir;
const REPO_SLUG = "mcyork/7segclock";
const PIO_ENV = "c3supermini";
const MAX_APP_BYTES = 0x1e0000;
const ESP_IMAGE_MAGIC = 0xe9;
const UPDATE_MARKER = "checkupdate";

const REL_APP = `.pio/build/${PIO_ENV}/firmware.bin`;
const REL_FACTORY = `.pio/build/${PIO_ENV}/firmware.factory.bin`;
const APP_BIN = join(REPO_ROOT, REL_APP);
const FACTORY_BIN = join(REPO_ROOT, REL_FACTORY);
const MAIN_CPP = join(REPO_ROOT, "src/main.cpp");
const MANIFEST = join(REPO_ROOT, "docs/manifest.json");
const DOCS_FACTORY = join(REPO_ROOT, "docs/firmware.factory.bin");
const DOCS_SHA = join(REPO_ROOT, "docs/firmware.factory.bin.sha256");

const LOCAL_GIT_MS = 15_000;
const NETWORK_GIT_MS = 30_000;
const PUSH_MS = 120_000;
const PIO_MS = 600_000;
const GH_MS = 180_000;

type Mode = { check: boolean; build: boolean; publish: boolean; dryRun: boolean; help: boolean };
type RunResult = { exitCode: number; stdout: string; stderr: string };
type CheckResult = { name: string; ok: boolean; detail: string };
type Artefacts = { app: Buffer; factory: Buffer; appSha: string; factorySha: string };
type ManifestPlan = { from: string; text: string; changed: boolean };
type ReleaseAsset = { name: string; size: number };
type ReleaseView = { url: string; tagName: string; isDraft: boolean; isPrerelease: boolean; assets: ReleaseAsset[] };

/** A failure of a named check. main() prints it as one `FAIL <check>: <reason>` line and exits 1. */
class ReleaseError extends Error {
  constructor(readonly check: string, message: string) {
    super(message);
  }
}

function fail(check: string, reason: string): never {
  throw new ReleaseError(check, reason);
}

// ---------------------------------------------------------------- subprocesses

const CHILD_ENV: Record<string, string | undefined> = { ...process.env, GIT_TERMINAL_PROMPT: "0", GH_PROMPT_DISABLED: "1" };

/** Renders an argv as a copy-pasteable shell line. For display only — nothing is ever run through a shell. */
function display(cmd: readonly string[]): string {
  return cmd.map((arg) => (/^[A-Za-z0-9_\-./:=@+,]+$/.test(arg) ? arg : `'${arg.replace(/'/g, `'\\''`)}'`)).join(" ");
}

function spawnOrFail(cmd: string[], timeoutMs: number, inherit: boolean) {
  try {
    return Bun.spawnSync(cmd, {
      cwd: REPO_ROOT,
      env: CHILD_ENV,
      stdin: "ignore",
      stdout: inherit ? "inherit" : "pipe",
      stderr: inherit ? "inherit" : "pipe",
      timeout: timeoutMs,
      killSignal: "SIGTERM",
    });
  } catch (err) {
    return fail("spawn", `${display(cmd)}: ${err instanceof Error ? err.message : String(err)}`);
  }
}

function exitCodeOrFail(cmd: string[], timeoutMs: number, proc: { exitCode: number | null; exitedDueToTimeout?: boolean; signalCode?: string | null }): number {
  if (proc.exitedDueToTimeout) fail("timeout", `${display(cmd)} did not finish within ${timeoutMs / 1000} s`);
  if (proc.exitCode === null) fail("spawn", `${display(cmd)} was killed by ${proc.signalCode ?? "an unknown signal"}`);
  return proc.exitCode;
}

/** Runs a command with captured output. Timeouts and signals fail; a non-zero exit is returned for the caller to judge. */
function run(cmd: string[], timeoutMs: number): RunResult {
  const proc = spawnOrFail(cmd, timeoutMs, false);
  const exitCode = exitCodeOrFail(cmd, timeoutMs, proc);
  return { exitCode, stdout: proc.stdout ? proc.stdout.toString() : "", stderr: proc.stderr ? proc.stderr.toString() : "" };
}

/** Runs a command with its output streamed to this terminal; returns the exit code. */
function runStreaming(cmd: string[], timeoutMs: number): number {
  return exitCodeOrFail(cmd, timeoutMs, spawnOrFail(cmd, timeoutMs, true));
}

function firstLine(text: string): string {
  return text.trim().split("\n")[0] ?? "";
}

/** Runs a command that must exit 0 and returns its trimmed stdout. */
function runOk(check: string, cmd: string[], timeoutMs: number): string {
  const res = run(cmd, timeoutMs);
  if (res.exitCode !== 0) fail(check, `${display(cmd)} exited ${res.exitCode}: ${firstLine(res.stderr) || firstLine(res.stdout)}`);
  return res.stdout.trim();
}

function resolvePio(): string {
  const fromEnv = process.env.PIO;
  if (fromEnv) return fromEnv;
  const penv = join(homedir(), ".platformio/penv/bin/pio");
  if (existsSync(penv)) return penv;
  return Bun.which("pio") ?? fail("pio", "pio not found (set PIO, or install PlatformIO at ~/.platformio/penv/bin/pio)");
}

// ---------------------------------------------------------------- files

function readText(check: string, path: string): string {
  if (!existsSync(path)) fail(check, `${path} does not exist`);
  return readFileSync(path, "utf8");
}

function readBinary(check: string, path: string): Buffer {
  if (!existsSync(path)) fail(check, `${path} does not exist (run --build)`);
  return readFileSync(path);
}

function sha256(data: Buffer): string {
  return createHash("sha256").update(data).digest("hex");
}

/** Writes via temp file + rename so an interrupted run never leaves a torn file in docs/. */
function writeAtomically(path: string, data: string | Buffer): void {
  const tmp = `${path}.tmp-${process.pid}`;
  writeFileSync(tmp, data);
  renameSync(tmp, path);
}

function copyAtomically(src: string, dest: string): void {
  const tmp = `${dest}.tmp-${process.pid}`;
  copyFileSync(src, tmp);
  renameSync(tmp, dest);
}

function readFirmwareVersion(): string {
  const text = readText("version", MAIN_CPP);
  const values = [...text.matchAll(/^[ \t]*#define[ \t]+FW_VERSION[ \t]+"([^"]*)"/gm)].map((m) => m[1] ?? "");
  if (values.length !== 1) fail("version", `expected exactly one #define FW_VERSION in src/main.cpp, found ${values.length}`);
  const version = values[0] ?? "";
  if (!/^\d+\.\d+\.\d+$/.test(version)) fail("version", `FW_VERSION "${version}" is not X.Y.Z`);
  return version;
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function parseManifest(text: string): Record<string, unknown> {
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch (err) {
    return fail("manifest", `docs/manifest.json is not valid JSON: ${err instanceof Error ? err.message : String(err)}`);
  }
  if (!isPlainObject(parsed)) fail("manifest", "docs/manifest.json is not a JSON object");
  if (typeof parsed.version !== "string") fail("manifest", "docs/manifest.json has no string \"version\" field");
  return parsed;
}

function manifestVersion(): string {
  const version = parseManifest(readText("manifest", MANIFEST)).version;
  return typeof version === "string" ? version : fail("manifest", "docs/manifest.json has no string \"version\" field");
}

function withoutVersion(manifest: Record<string, unknown>): string {
  return JSON.stringify(Object.entries(manifest).filter(([key]) => key !== "version"));
}

/** Plans a textual edit of the one "version" value, so every other byte of the file is preserved. */
function planManifestUpdate(version: string): ManifestPlan {
  const original = readText("manifest", MANIFEST);
  const before = parseManifest(original);
  const from = String(before.version);
  if (from === version) return { from, text: original, changed: false };
  const keyCount = original.match(/"version"\s*:/g)?.length ?? 0;
  if (keyCount !== 1) fail("manifest", `expected exactly one "version" key in docs/manifest.json, found ${keyCount}`);
  const text = original.replace(/("version"\s*:\s*)"[^"\\]*"/, (_match: string, prefix: string) => `${prefix}"${version}"`);
  const after = parseManifest(text);
  if (after.version !== version) fail("manifest", "rewriting the version field did not produce the expected value");
  if (withoutVersion(after) !== withoutVersion(before)) fail("manifest", "rewriting the version field changed other fields");
  return { from, text, changed: true };
}

// ---------------------------------------------------------------- artefacts

function requireStrings(check: string, label: string, buf: Buffer, needles: string[]): void {
  for (const needle of needles) {
    if (!buf.includes(Buffer.from(needle, "ascii"))) fail(check, `${label} does not contain the string "${needle}"`);
  }
}

/** The FW_VERSION literal is a NUL-terminated C string. Searching for "\0X.Y.Z\0" rather
 *  than the bare digits means a timezone rule like "M3.2.0" or a library version cannot
 *  satisfy the check by accident. */
function requireVersionLiteral(check: string, label: string, buf: Buffer, version: string): void {
  if (!buf.includes(Buffer.from(`\0${version}\0`, "ascii"))) fail(check, `${label} does not contain FW_VERSION "${version}" as a string literal`);
}

/** sha256 over the sources the binary was built from, as they are on disk (working tree) or
 *  as committed at a ref — so --publish can prove the tag's sources are what --build compiled. */
const BUILD_INPUTS = ["platformio.ini", "src/main.cpp", "src/settings.h", "src/ticker.h", "src/webui.h", "src/geometryui.h"];
const BUILT_FROM = join(REPO_ROOT, `.pio/build/${PIO_ENV}/built-from.sha256`);

function sourcesDigest(reader: (path: string) => Buffer): string {
  const h = createHash("sha256");
  for (const p of BUILD_INPUTS) { h.update(p); h.update("\0"); h.update(reader(p)); h.update("\0"); }
  return h.digest("hex");
}

function workingTreeDigest(): string {
  return sourcesDigest((p) => readBinary("sources", join(REPO_ROOT, p)));
}

function committedDigest(ref: string): string {
  return sourcesDigest((p) => {
    const res = run(["git", "show", `${ref}:${p}`], LOCAL_GIT_MS);
    if (res.exitCode !== 0) fail("sources", `git show ${ref}:${p} exited ${res.exitCode}: ${firstLine(res.stderr)}`);
    return Buffer.from(res.stdout, "utf8");
  });
}

function hex(n: number): string {
  return `0x${n.toString(16).toUpperCase()}`;
}

/** Verifies the two .pio images against the frozen contract and against each other. */
function verifyArtefacts(version: string): Artefacts {
  const app = readBinary("app-image", APP_BIN);
  if (app.length > MAX_APP_BYTES) fail("app-size", `firmware.bin is ${app.length} bytes (${hex(app.length)}), over the ${hex(MAX_APP_BYTES)} OTA slot`);
  if (app.length === 0 || app[0] !== ESP_IMAGE_MAGIC) fail("app-magic", `firmware.bin starts with ${app.length ? hex(app[0] ?? 0) : "nothing"}, not the ESP image magic 0xE9`);
  requireStrings("app-image", "firmware.bin", app, [UPDATE_MARKER]);
  requireVersionLiteral("app-image", "firmware.bin", app, version);

  const factory = readBinary("factory-image", FACTORY_BIN);
  requireStrings("factory-image", "firmware.factory.bin", factory, [UPDATE_MARKER]);
  requireVersionLiteral("factory-image", "firmware.factory.bin", factory, version);
  if (factory.length <= app.length) fail("factory-image", `firmware.factory.bin (${factory.length} B) is not larger than firmware.bin (${app.length} B)`);
  if (!factory.includes(app)) fail("factory-image", "firmware.factory.bin does not contain firmware.bin verbatim — the two images are from different builds");

  return { app, factory, appSha: sha256(app), factorySha: sha256(factory) };
}

function printArtefacts(a: Artefacts): void {
  const pct = ((a.app.length / MAX_APP_BYTES) * 100).toFixed(1);
  printTable(
    ["image", "bytes", "hex", "of OTA slot", "sha256"],
    [
      ["firmware.bin", String(a.app.length), hex(a.app.length), `${pct}%`, a.appSha],
      ["firmware.factory.bin", String(a.factory.length), hex(a.factory.length), "n/a", a.factorySha],
    ],
  );
}

// ---------------------------------------------------------------- output

function printTable(headers: string[], rows: string[][]): void {
  const widths = headers.map((h, i) => Math.max(h.length, ...rows.map((r) => (r[i] ?? "").length)));
  const line = (cells: string[]): string => cells.map((c, i) => c.padEnd(widths[i] ?? 0)).join("  ").trimEnd();
  console.log(line(headers));
  console.log(widths.map((w) => "-".repeat(w)).join("  "));
  for (const row of rows) console.log(line(row));
}

function printChecks(results: CheckResult[]): void {
  printTable(["check", "result", "detail"], results.map((r) => [r.name, r.ok ? "PASS" : "FAIL", r.detail]));
}

/** Runs one check, turning a ReleaseError into a FAIL row so the table shows every problem at once. */
function probe(name: string, body: () => string): CheckResult {
  try {
    return { name, ok: true, detail: body() };
  } catch (err) {
    if (err instanceof ReleaseError) return { name, ok: false, detail: err.message };
    throw err;
  }
}

// ---------------------------------------------------------------- read-only checks

function checkBranch(): string {
  const res = run(["git", "symbolic-ref", "--short", "-q", "HEAD"], LOCAL_GIT_MS);
  if (res.exitCode === 1) fail("branch", "HEAD is detached; check out main");
  if (res.exitCode !== 0) fail("branch", `git symbolic-ref exited ${res.exitCode}: ${firstLine(res.stderr)}`);
  const branch = res.stdout.trim();
  if (branch !== "main") fail("branch", `on branch ${branch}, releases are cut from main`);
  return "main";
}

function checkCleanTree(hint: string): string {
  // Not runOk: its trim() would eat the leading status column (" M path") of the first line.
  const res = run(["git", "status", "--porcelain"], LOCAL_GIT_MS);
  if (res.exitCode !== 0) fail("clean-tree", `git status exited ${res.exitCode}: ${firstLine(res.stderr)}`);
  const lines = res.stdout.split("\n").filter((l) => l.length > 0);
  if (lines.length > 0) {
    const shown = lines.slice(0, 5).map((l) => l.slice(3)).join(", ");
    fail("clean-tree", `${lines.length} dirty path(s): ${shown}${lines.length > 5 ? ", ..." : ""}${hint}`);
  }
  return "no uncommitted changes";
}

function checkOrigin(): string {
  const url = runOk("origin", ["git", "remote", "get-url", "origin"], LOCAL_GIT_MS);
  const ok = /^(?:https:\/\/github\.com\/|git@github\.com:|ssh:\/\/git@github\.com\/)mcyork\/7segclock(?:\.git)?\/?$/i.test(url);
  if (!ok) fail("origin", `origin is ${url}, the fielded updater only reads ${REPO_SLUG}`);
  return url;
}

function checkTagLocal(tag: string): string {
  const res = run(["git", "rev-parse", "-q", "--verify", `refs/tags/${tag}`], LOCAL_GIT_MS);
  if (res.exitCode === 0) fail("tag-local", `tag ${tag} already exists locally — bump FW_VERSION`);
  if (res.exitCode !== 1) fail("tag-local", `git rev-parse exited ${res.exitCode}: ${firstLine(res.stderr)}`);
  return `${tag} not present`;
}

/** Returns true if the tag exists on origin; fails if origin cannot be queried. */
function remoteTagExists(tag: string, check: string): boolean {
  const res = run(["git", "ls-remote", "--tags", "origin", `refs/tags/${tag}`], NETWORK_GIT_MS);
  if (res.exitCode !== 0) fail(check, `cannot reach origin: ${firstLine(res.stderr)}`);
  return res.stdout.trim().length > 0;
}

function checkTagRemote(tag: string): string {
  if (remoteTagExists(tag, "tag-remote")) fail("tag-remote", `tag ${tag} already exists on origin — bump FW_VERSION`);
  return `${tag} not on origin`;
}

function checkOriginMain(): string {
  const out = runOk("origin-main", ["git", "ls-remote", "origin", "refs/heads/main"], NETWORK_GIT_MS);
  const sha = /^([0-9a-f]{40,64})\trefs\/heads\/main$/m.exec(out)?.[1];
  if (!sha) fail("origin-main", `could not read origin/main from ls-remote output: ${firstLine(out) || "(empty)"}`);
  const res = run(["git", "merge-base", "--is-ancestor", sha, "HEAD"], LOCAL_GIT_MS);
  if (res.exitCode === 1) fail("origin-main", "origin/main has commits not in HEAD — pull first");
  if (res.exitCode !== 0) fail("origin-main", `origin/main ${sha.slice(0, 12)} not present locally — pull first`);
  return `HEAD contains origin/main ${sha.slice(0, 12)}`;
}

function checkManifest(version: string): string {
  const found = manifestVersion();
  if (found !== version) fail("manifest", `manifest says ${found}, FW_VERSION is ${version} (run --build)`);
  return `version ${found}`;
}

/** Three numeric parts, strictly greater. Mirrors isNewer() in the firmware. */
function isNewer(a: string, b: string): boolean {
  const pa = a.split(".").map(Number), pb = b.split(".").map(Number);
  for (let i = 0; i < 3; i++) { if ((pa[i] ?? 0) !== (pb[i] ?? 0)) return (pa[i] ?? 0) > (pb[i] ?? 0); }
  return false;
}

/** FW_VERSION must be newer than whatever GitHub already calls latest: fielded devices
 *  compare against that tag, and a release that is not newer is one nobody can install. */
function checkNewerThanLatest(version: string): string {
  const gh = Bun.which("gh") ?? fail("latest", "gh CLI not found on PATH");
  const res = run([gh, "api", `repos/${REPO_SLUG}/releases/latest`, "--jq", ".tag_name"], GH_MS);
  if (res.exitCode !== 0) {
    if (/404|Not Found/i.test(res.stderr + res.stdout)) return "no release published yet";
    fail("latest", `gh api releases/latest exited ${res.exitCode}: ${firstLine(res.stderr)}`);
  }
  const raw = res.stdout.trim();
  const latest = raw.startsWith("v") ? raw.slice(1) : raw;
  if (!/^\d+\.\d+\.\d+$/.test(latest)) fail("latest", `latest release tag "${raw}" is not vX.Y.Z`);
  if (!isNewer(version, latest)) fail("latest", `FW_VERSION ${version} is not newer than the published latest ${raw} — bump it`);
  return `${version} > ${raw}`;
}

/** The binary in .pio/ must have been built from exactly the sources the release commit carries. */
function checkBuiltFromHead(): string {
  const recorded = readText("built-from", BUILT_FROM).trim();
  if (!/^[0-9a-f]{64}$/.test(recorded)) fail("built-from", `${BUILT_FROM} is not a sha256 (run --build)`);
  const head = committedDigest("HEAD");
  if (recorded !== head) fail("built-from", "the .pio/ images were not built from HEAD's sources — run --build, commit, then --publish");
  return `sources match HEAD (${head.slice(0, 12)})`;
}

/** The --check table. Never mutates anything. Returns true if every check passed. */
function runCheckTable(): boolean {
  let version = "";
  const versionRow = probe("version", () => (version = readFirmwareVersion()));
  if (!versionRow.ok) {
    printChecks([versionRow]);
    return false;
  }
  const tag = `v${version}`;
  const results: CheckResult[] = [
    versionRow,
    probe("branch", checkBranch),
    probe("clean-tree", () => checkCleanTree("")),
    probe("origin", checkOrigin),
    probe("tag-local", () => checkTagLocal(tag)),
    probe("tag-remote", () => checkTagRemote(tag)),
    probe("origin-main", checkOriginMain),
    probe("manifest", () => checkManifest(version)),
    probe("latest", () => checkNewerThanLatest(version)),
  ];
  printChecks(results);
  return results.every((r) => r.ok);
}

// ---------------------------------------------------------------- --build

function stageDocs(version: string, a: Artefacts, dryRun: boolean): void {
  const plan = planManifestUpdate(version);
  const shaLine = `${a.factorySha}  firmware.factory.bin\n`;
  if (dryRun) {
    // A dry run must leave the tracked tree untouched, so the docs/ writes are only described.
    // The built-from record lives in the gitignored .pio/ and is written even here, so a
    // dry run followed by a commit and --publish still has something true to check.
    writeAtomically(BUILT_FROM, `${workingTreeDigest()}\n`);
    console.log(`DRY-RUN would: copy ${REL_FACTORY} -> docs/firmware.factory.bin`);
    console.log(plan.changed ? `DRY-RUN would: set docs/manifest.json version ${plan.from} -> ${version}` : `DRY-RUN would: leave docs/manifest.json (already ${version})`);
    console.log(`DRY-RUN would: write docs/firmware.factory.bin.sha256 = ${a.factorySha}`);
    return;
  }
  copyAtomically(FACTORY_BIN, DOCS_FACTORY);
  console.log(`copied ${REL_FACTORY} -> docs/firmware.factory.bin`);
  if (plan.changed) {
    writeAtomically(MANIFEST, plan.text);
    console.log(`set docs/manifest.json version ${plan.from} -> ${version}`);
  } else {
    console.log(`docs/manifest.json already at ${version}, left untouched`);
  }
  writeAtomically(DOCS_SHA, shaLine);
  console.log(`wrote docs/firmware.factory.bin.sha256 = ${a.factorySha}`);
  // Which sources this binary came from, so --publish can refuse a stale .pio/.
  writeAtomically(BUILT_FROM, `${workingTreeDigest()}\n`);

  if (sha256(readFileSync(DOCS_FACTORY)) !== a.factorySha) fail("docs-factory", "docs/firmware.factory.bin does not match the build after copying");
  if (manifestVersion() !== version) fail("manifest", "docs/manifest.json does not carry FW_VERSION after writing");
}

function build(version: string, dryRun: boolean): void {
  const cmd = [resolvePio(), "run", "-e", PIO_ENV];
  console.log(`$ ${display(cmd)}`);
  const code = runStreaming(cmd, PIO_MS);
  if (code !== 0) fail("build", `pio run exited ${code}`);
  const artefacts = verifyArtefacts(version);
  console.log(`\nverified: magic 0xE9, <= ${hex(MAX_APP_BYTES)}, contains "${UPDATE_MARKER}" and "${version}", app embedded in factory image`);
  printArtefacts(artefacts);
  console.log("");
  stageDocs(version, artefacts, dryRun);
  if (!dryRun) console.log("\nnext: review and commit docs/ (git add docs && git commit), then run: bun Release.ts --publish");
}

// ---------------------------------------------------------------- --publish

function checkDocsFactory(a: Artefacts): string {
  const docs = readBinary("docs-factory", DOCS_FACTORY);
  if (sha256(docs) !== a.factorySha) fail("docs-factory", "docs/firmware.factory.bin is from a different build — run --build and commit");
  return `matches build ${a.factorySha.slice(0, 12)}`;
}

function checkDocsSha(a: Artefacts): string {
  const text = readText("docs-sha256", DOCS_SHA);
  // Accept sha256sum/shasum output run from docs/ or from the repo root, text or binary mode; only the hash is judged.
  const recorded = /^([0-9a-f]{64})(?: [ *](?:docs\/)?firmware\.factory\.bin)?\s*$/.exec(text)?.[1];
  if (!recorded) fail("docs-sha256", "docs/firmware.factory.bin.sha256 is not '<sha256>  firmware.factory.bin'");
  if (recorded !== a.factorySha) fail("docs-sha256", "docs/firmware.factory.bin.sha256 does not match the build — run --build and commit");
  return "matches";
}

function checkGh(): string {
  const gh = Bun.which("gh") ?? fail("gh", "gh CLI not found on PATH");
  runOk("gh-auth", [gh, "auth", "status"], NETWORK_GIT_MS);
  return "authenticated";
}

type Preflight = { results: CheckResult[]; artefacts: Artefacts | null; notes: string | null };

function verifiedArtefactsOrNull(version: string, results: CheckResult[]): Artefacts | null {
  try {
    const a = verifyArtefacts(version);
    results.push({ name: "artefacts", ok: true, detail: `app ${a.app.length} B (${hex(a.app.length)}), factory ${a.factory.length} B` });
    return a;
  } catch (err) {
    if (!(err instanceof ReleaseError)) throw err;
    results.push({ name: "artefacts", ok: false, detail: `${err.check}: ${err.message}` });
    return null;
  }
}

/** Everything --publish needs before it may touch anything, including the release notes. Read-only. */
function publishPreflight(version: string, builtThisRun: boolean): Preflight {
  const tag = `v${version}`;
  const cleanHint = builtThisRun
    ? " — commit the docs/ changes first (git add docs && git commit), then run --publish on its own so the committed installer image is exactly what ships"
    : " — commit the docs/ changes first (git add docs && git commit), then run --publish";
  const results: CheckResult[] = [
    probe("branch", checkBranch),
    probe("clean-tree", () => checkCleanTree(cleanHint)),
    probe("origin", checkOrigin),
    probe("tag-local", () => checkTagLocal(tag)),
    probe("tag-remote", () => checkTagRemote(tag)),
    probe("origin-main", checkOriginMain),
  ];
  const artefacts = verifiedArtefactsOrNull(version, results);
  results.push(probe("manifest", () => checkManifest(version)));
  results.push(probe("latest", () => checkNewerThanLatest(version)));
  results.push(probe("built-from", checkBuiltFromHead));
  let notes: string | null = null;
  if (artefacts) {
    results.push(
      probe("docs-factory", () => checkDocsFactory(artefacts)),
      probe("docs-sha256", () => checkDocsSha(artefacts)),
      probe("notes", () => {
        const prev = previousTag();
        notes = releaseNotes(prev, artefacts);
        return `commits since ${prev ?? "start of history"}`;
      }),
    );
  } else {
    results.push({ name: "docs-factory", ok: false, detail: "skipped: build artefacts failed verification" });
  }
  results.push(probe("gh", checkGh));
  return { results, artefacts, notes };
}

function previousTag(): string | null {
  const res = run(["git", "describe", "--tags", "--abbrev=0", "--match", "v[0-9]*", "HEAD"], LOCAL_GIT_MS);
  return res.exitCode === 0 ? res.stdout.trim() : null;
}

function releaseNotes(prevTag: string | null, a: Artefacts): string {
  const range = prevTag ? [`${prevTag}..HEAD`] : ["-n", "50", "HEAD"];
  const log = runOk("notes", ["git", "log", "--oneline", "--no-decorate", "--no-color", ...range], LOCAL_GIT_MS);
  const lines = log.split("\n").filter((l) => l.length > 0);
  if (lines.length === 0) fail("notes", `no commits since ${prevTag ?? "the start of history"}`);
  return [
    prevTag ? `## Changes since ${prevTag}` : "## Changes",
    "",
    ...lines.map((l) => `- ${l}`),
    "",
    "## Over-the-air updates",
    "",
    "Fielded 7segclock devices check this repository's latest release and install the asset named `firmware.bin` from it over the air. Do not rename, remove or replace that asset, and never mark a release that lacks it as Latest. `firmware.factory.bin` is attached for manual USB flashing only; the browser installer serves its own copy from GitHub Pages.",
    "",
    "## SHA-256",
    "",
    "```",
    `${a.appSha}  firmware.bin`,
    `${a.factorySha}  firmware.factory.bin`,
    "```",
    "",
  ].join("\n");
}

function isReleaseView(value: unknown): value is ReleaseView {
  if (!isPlainObject(value)) return false;
  const { url, tagName, isDraft, isPrerelease, assets } = value;
  return (
    typeof url === "string" &&
    typeof tagName === "string" &&
    typeof isDraft === "boolean" &&
    typeof isPrerelease === "boolean" &&
    Array.isArray(assets) &&
    assets.every((x: unknown) => isPlainObject(x) && typeof x.name === "string" && typeof x.size === "number")
  );
}

/** Reads the published release back and proves it meets the frozen contract. Returns the release URL. */
function verifyPublishedRelease(gh: string, tag: string, appBytes: number): string {
  const json = runOk("release-contract", [gh, "release", "view", tag, "--repo", REPO_SLUG, "--json", "url,tagName,isDraft,isPrerelease,assets"], GH_MS);
  let parsed: unknown;
  try {
    parsed = JSON.parse(json);
  } catch (err) {
    return fail("release-contract", `gh release view returned invalid JSON: ${err instanceof Error ? err.message : String(err)}`);
  }
  if (!isReleaseView(parsed)) fail("release-contract", "gh release view returned an unexpected shape");
  if (parsed.tagName !== tag) fail("release-contract", `release tag is ${parsed.tagName}, expected ${tag}`);
  if (parsed.isDraft) fail("release-contract", `${tag} is a draft`);
  if (parsed.isPrerelease) fail("release-contract", `${tag} is a prerelease`);
  const asset = parsed.assets.find((x) => x.name === "firmware.bin");
  if (!asset) fail("release-contract", `${tag} has no asset named firmware.bin`);
  if (asset.size !== appBytes) fail("release-contract", `firmware.bin asset is ${asset.size} B, local file is ${appBytes} B`);
  if (asset.size > MAX_APP_BYTES) fail("release-contract", `firmware.bin asset exceeds ${hex(MAX_APP_BYTES)}`);

  // GitHub's "latest" pointer can trail the create call briefly; three bounded attempts, 3 s apart.
  let latest = "";
  for (let attempt = 1; attempt <= 3; attempt++) {
    latest = runOk("release-contract", [gh, "api", `repos/${REPO_SLUG}/releases/latest`, "--jq", ".tag_name"], GH_MS);
    if (latest === tag) return parsed.url;
    if (attempt < 3) Bun.sleepSync(3000);
  }
  return fail("release-contract", `releases/latest is ${latest || "(empty)"}, expected ${tag}`);
}

function pushOrRollBack(tag: string, pushCmd: string[], releaseCmdText: string): void {
  const push = run(pushCmd, PUSH_MS);
  if (push.exitCode === 0) return;
  const reason = `git push exited ${push.exitCode}: ${firstLine(push.stderr)}`;
  let onOrigin: boolean;
  try {
    onOrigin = remoteTagExists(tag, "push");
  } catch (err) {
    const why = err instanceof Error ? err.message : String(err);
    return fail("push", `${reason}; origin could not be queried afterwards (${why}). Local tag ${tag} kept — if 'git ls-remote --tags origin ${tag}' is empty, remove it with 'git tag -d ${tag}'`);
  }
  if (onOrigin) fail("push", `${reason}; but ${tag} reached origin — inspect, then create the release with: ${releaseCmdText}`);
  runOk("push", ["git", "tag", "-d", tag], LOCAL_GIT_MS);
  fail("push", `${reason}; local tag ${tag} removed, nothing was published — fix the push and re-run --publish`);
}

function publish(version: string, dryRun: boolean, builtThisRun: boolean): void {
  const tag = `v${version}`;
  const { results, artefacts, notes } = publishPreflight(version, builtThisRun);
  printChecks(results);
  const firstFailure = results.find((r) => !r.ok);

  const notesDir = mkdtempSync(join(tmpdir(), "7segclock-release-"));
  const notesPath = join(notesDir, "notes.md");
  let keepNotes = false;
  try {
    const gh = Bun.which("gh") ?? "gh";
    const tagCmd = ["git", "tag", "-a", tag, "-m", `7segclock ${version}`];
    const pushCmd = ["git", "push", "origin", "main", "--follow-tags"];
    const releaseCmd = [gh, "release", "create", tag, REL_APP, REL_FACTORY, "--repo", REPO_SLUG, "--title", `7segclock ${version}`, "--notes-file", notesPath, "--latest", "--verify-tag"];
    const releaseCmdText = display(["gh", ...releaseCmd.slice(1)]);

    if (dryRun) {
      if (firstFailure) console.log(`\nDRY-RUN: preflight failed (${firstFailure.name}); a real --publish would refuse here. Planned commands, for reference:`);
      console.log(`\nDRY-RUN would run: ${display(tagCmd)}`);
      console.log(`DRY-RUN would run: ${display(pushCmd)}`);
      console.log(`DRY-RUN would run: ${releaseCmdText}`);
      console.log(`DRY-RUN would verify: gh release view ${tag} (not draft, not prerelease, firmware.bin <= ${hex(MAX_APP_BYTES)}) and releases/latest == ${tag}`);
      console.log(`\nDRY-RUN notes file:\n${notes ?? "(not generated — see the artefacts/notes rows above)"}`);
      if (firstFailure) fail(firstFailure.name, firstFailure.detail);
      return;
    }

    if (firstFailure) fail(firstFailure.name, firstFailure.detail);
    if (!artefacts || notes === null) return fail("artefacts", "build artefacts unavailable");
    writeFileSync(notesPath, notes);

    console.log(`\n$ ${display(tagCmd)}`);
    runOk("tag", tagCmd, LOCAL_GIT_MS);
    console.log(`$ ${display(pushCmd)}`);
    pushOrRollBack(tag, pushCmd, releaseCmdText);
    console.log(`$ ${releaseCmdText}`);
    const created = run(releaseCmd, GH_MS);
    if (created.exitCode !== 0) {
      keepNotes = true;
      fail("release", `gh release create exited ${created.exitCode}: ${firstLine(created.stderr)}. ${tag} is already on origin (do not delete it); retry with: ${releaseCmdText}`);
    }
    const url = verifyPublishedRelease(gh, tag, artefacts.app.length);
    console.log(`\npublished ${tag}: ${url}`);
  } finally {
    if (!keepNotes) rmSync(notesDir, { recursive: true, force: true });
  }
}

// ---------------------------------------------------------------- main

const USAGE = `usage: bun Release.ts [--check | --build | --publish] [--dry-run]

  --check     read-only preflight: version, branch, clean tree, origin, tags, manifest (default)
  --build     pio run, verify both images, stage docs/firmware.factory.bin + manifest + sha256
  --publish   tag v<FW_VERSION>, push main with tags, create the GitHub Release, verify it
  --dry-run   print tag/push/gh and docs/ writes instead of performing them
  --help      this text

flow: --check, bump FW_VERSION, --build, commit docs/, --publish`;

function parseArgs(argv: string[]): Mode {
  const mode: Mode = { check: false, build: false, publish: false, dryRun: false, help: false };
  for (const arg of argv) {
    switch (arg) {
      case "--check": mode.check = true; break;
      case "--build": mode.build = true; break;
      case "--publish": mode.publish = true; break;
      case "--dry-run": mode.dryRun = true; break;
      case "--help": case "-h": mode.help = true; break;
      default: fail("args", `unknown flag ${arg}`);
    }
  }
  if (!mode.check && !mode.build && !mode.publish) mode.check = true;
  return mode;
}

function main(): number {
  const mode = parseArgs(Bun.argv.slice(2));
  if (mode.help) {
    console.log(USAGE);
    return 0;
  }
  if (mode.check && !runCheckTable()) {
    console.error("FAIL check: one or more checks failed (see table)");
    return 1;
  }
  if (!mode.build && !mode.publish) return 0;
  const version = readFirmwareVersion();
  if (mode.build) build(version, mode.dryRun);
  if (mode.publish) publish(version, mode.dryRun, mode.build);
  return 0;
}

try {
  process.exit(main());
} catch (err) {
  if (err instanceof ReleaseError) {
    console.error(`FAIL ${err.check}: ${err.message}`);
  } else {
    console.error(`FAIL unexpected: ${err instanceof Error ? (err.stack ?? err.message) : String(err)}`);
  }
  process.exit(1);
}
