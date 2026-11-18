#!/usr/bin/env node
// Read-only regression for the pinned WeFlow 6.3.1 minified bundles.
// Uses synthetic data and extracts the real backend loop and renderer VF/HF/JT.
// Usage: node test-tool-stream.mjs --before /path/original.asar --after /path/patched.asar
import assert from 'node:assert/strict'
import { openSync, readSync, closeSync } from 'node:fs'
import { createHash } from 'node:crypto'
import vm from 'node:vm'

function archive(path) {
  const fd = openSync(path, 'r')
  const read = (offset, length) => {
    const result = Buffer.alloc(length)
    assert.equal(readSync(fd, result, 0, length, offset), length)
    return result
  }
  const prefix = read(0, 16)
  const header = JSON.parse(read(16, prefix.readUInt32LE(12)))
  const base = 8 + prefix.readUInt32LE(4)
  return {
    text(path) {
      let node = header
      for (const segment of path.split('/')) node = node.files[segment]
      assert.ok(node && !node.unpacked, `Missing packed entry: ${path}`)
      return read(base + Number(node.offset), node.size).toString('utf8')
    },
    rendererPath() {
      const names = Object.keys(header.files.dist.files.assets.files)
        .filter(name => /^AgentPage-.*\.js$/.test(name))
      assert.equal(names.length, 1)
      return `dist/assets/${names[0]}`
    },
    close() { closeSync(fd) },
  }
}

function between(source, start, end) {
  const i = source.indexOf(start)
  assert.ok(i >= 0, `Missing start marker: ${start}`)
  const j = source.indexOf(end, i)
  assert.ok(j > i, `Missing end marker: ${end}`)
  return source.slice(i, j)
}

function rendererRuntime(source) {
  const runtime = vm.createContext({
    ReadableStream,
    TransformStream,
    // The actual mT class only depends on this generic error base. No parser
    // errors are stubbed: its exact getToolInvocation check runs below.
    dm: class extends Error {
      constructor({ name, message }) { super(message); this.name = name }
    },
  })
  const errorClass = between(source, 'var uT=`AI_UIMessageStreamError`', ';var hT=')
  const parser = between(source, 'function VT(', 'async function YT(')
  const transport = between(source, 'function zF(', 'function UF(')
  vm.runInContext(`${errorClass};${parser};${transport};this.api={JT,qT,VF,HF}`, runtime)
  return runtime.api
}

function backendLoop(source, kind) {
  const marker = 'e.type===`tool-input-error`&&'
  const branch = source.indexOf(marker)
  assert.ok(branch >= 0, `${kind}: missing error branch`)
  assert.equal(source.indexOf(marker, branch + marker.length), -1, `${kind}: ambiguous branch`)
  const iterator = source.lastIndexOf('for await(let e of ', branch)
  const startMarker = ')){if(e.type===`start-step`'
  const start = source.indexOf(startMarker, iterator)
  assert.ok(start > iterator && start < branch)
  const end = source.indexOf('}}let ', branch)
  assert.ok(end > branch)
  // Include the inner conditional close, excluding the for-await loop close.
  const body = source.slice(start + 3, end + 1)
  const fn = new Function('chunks', 'ctx', `with(ctx){for(const e of chunks){${body}}}`)
  return {
    sha256: createHash('sha256').update(body).digest('hex'),
    run(chunks, { hidden = false } = {}) {
      const output = [], retryTools = new Set(), suppressedIds = new Set()
      const registry = { search_raw_messages: {}, read_memory: {} }
      if (hidden) registry.request_tools = {}
      const available = () => ({ search_raw_messages: {}, read_memory: {} })
      const emit = chunk => output.push(structuredClone(chunk))
      const trim = value => String(value || '').trim()
      const ctx = {
        D: new Set(Object.keys(available())), O: suppressedIds,
        Q: trim, b: '', x: [], S: false,
        Fe: () => [], je: () => [], ze: () => [],
        // Main and worker are separately compiled copies with different names.
        ei: retryTools, ri: retryTools, Jn: registry, Zn: registry,
        Mi: null, Fi: null, Pi: emit, Li: emit,
        Ti: available,
        E: () => {}, C: () => {}, Ai: '', Ni: '', ji: '',
        Oi: kind === 'main' ? '' : available,
      }
      fn(structuredClone(chunks), ctx)
      return { output, retryTools, suppressedIds }
    },
  }
}

async function consume(api, chunks) {
  const state = api.qT({ messageId: 'synthetic-assistant' })
  const normalizer = new api.HF()
  const received = [], invoked = []
  let writes = 0
  const normalized = chunks.flatMap(chunk => {
    const valid = api.VF(chunk)
    return valid ? normalizer.accept(valid) : []
  })
  const stream = new ReadableStream({
    start(controller) {
      for (const chunk of normalized) controller.enqueue(chunk)
      controller.close()
    },
  })
  const output = api.JT({
    stream,
    runUpdateMessageJob: async job => job({ state, write: () => { writes++ } }),
    onToolCall: async ({ toolCall }) => invoked.push(toolCall.toolName),
    onError: error => { throw error },
  })
  for await (const chunk of output) received.push(chunk)
  return { state, received, invoked, writes }
}

const callId = 'call_synthetic_unavailable'
const errorText = "AI_NoSuchToolError: Model tried to call unavailable tool 'request_tools'. Available tools: read_raw_timeline, search_raw_messages, read_memory."
const invalidInput = {
  type: 'tool-input-error', toolCallId: callId, toolName: 'request_tools',
  input: { tools: ['read_raw_messages'] }, dynamic: true, errorText,
}
const invalidOutput = { type: 'tool-output-error', toolCallId: callId, errorText }
const recovery = [
  { type: 'finish-step' },
  { type: 'start-step' },
  { type: 'tool-input-available', toolCallId: 'call_synthetic_known', toolName: 'search_raw_messages', input: { query: 'synthetic query' } },
  { type: 'tool-output-available', toolCallId: 'call_synthetic_known', output: { success: true, count: 1 } },
  { type: 'text-start', id: 'synthetic-text' },
  { type: 'text-delta', id: 'synthetic-text', delta: 'Synthetic answer after tool recovery.' },
  { type: 'text-end', id: 'synthetic-text' },
  { type: 'finish-step' },
  { type: 'finish', finishReason: 'stop' },
]

async function validate(path, expectedFixed) {
  const asar = archive(path)
  try {
    const renderer = asar.text(asar.rendererPath())
    const api = rendererRuntime(renderer)
    const passed = []
    for (const kind of ['main', 'worker']) {
      const source = asar.text(kind === 'main' ? 'dist-electron/main.js' : 'dist-electron/agentRunWorker.js')
      const loop = backendLoop(source, kind)
      for (const hidden of [false, true]) {
        const chunks = [
          { type: 'start', messageId: 'synthetic-assistant' },
          { type: 'start-step' },
          ...(hidden ? [
            { type: 'tool-input-start', toolCallId: callId, toolName: 'request_tools' },
            { type: 'tool-input-delta', toolCallId: callId, inputTextDelta: '{"tools":["read_raw_messages"]}' },
          ] : []),
          invalidInput, invalidOutput, ...recovery,
        ]
        const run = loop.run(chunks, { hidden })
        assert.ok(run.retryTools.has('request_tools'), 'Retry tracking must survive')
        assert.equal(run.output.some(chunk => chunk.type === 'tool-input-error'), expectedFixed)
        assert.equal(run.output.filter(chunk => chunk.type === 'tool-output-error').length, 1)
        if (hidden) {
          assert.ok(run.suppressedIds.has(callId))
          assert.ok(!run.output.some(chunk => chunk.type === 'tool-input-start' || chunk.type === 'tool-input-delta'))
        }
        if (expectedFixed) {
          const result = await consume(api, run.output)
          const parts = Array.from(result.state.message.parts)
          const failed = parts.find(part => part.toolCallId === callId)
          assert.equal(failed.state, 'output-error')
          assert.equal(failed.toolName, 'request_tools')
          assert.equal(failed.errorText, errorText)
          const known = parts.find(part => part.toolCallId === 'call_synthetic_known')
          assert.equal(known.state, 'output-available')
          assert.equal(known.output.count, 1)
          assert.equal(parts.find(part => part.type === 'text').text, 'Synthetic answer after tool recovery.')
          assert.equal(result.state.finishReason, 'stop')
          assert.deepEqual(result.invoked, ['search_raw_messages'], 'Invalid tool must not execute')
        } else {
          await assert.rejects(consume(api, run.output), {
            name: 'AI_UIMessageStreamError',
            message: `No tool invocation found for tool call ID "${callId}".`,
          })
        }
        passed.push(`${kind}:${hidden ? 'hidden-known' : 'unavailable'}:${expectedFixed ? 'recovered' : 'reproduced'}`)
      }
      // Valid tools remain unchanged in both binaries.
      const normal = loop.run([{ type: 'start-step' }, ...recovery])
      const result = await consume(api, normal.output)
      assert.equal(result.state.finishReason, 'stop')
      passed.push(`${kind}:normal-tools`)
    }
    // The renderer still rejects true orphan output chunks. No permissive UI fix.
    await assert.rejects(consume(api, [invalidOutput]), /No tool invocation found/)
    assert.equal(api.VF({ ...invalidInput, input: undefined }).type, 'tool-input-error')
    const malformed = { ...invalidInput }
    delete malformed.input
    assert.equal(api.VF(malformed), null)
    passed.push('renderer:strict-orphan-and-input-validation')
    return { path, expectedFixed, rendererSha256: createHash('sha256').update(renderer).digest('hex'), passed }
  } finally {
    asar.close()
  }
}

const args = process.argv.slice(2)
const paths = new Map()
while (args.length) {
  const flag = args.shift(), path = args.shift()
  assert.ok(['--before', '--after'].includes(flag) && path, 'Expected --before ARCHIVE and/or --after ARCHIVE')
  paths.set(flag, path)
}
assert.ok(paths.size, 'Supply --before and/or --after ASAR paths')
for (const [flag, path] of paths) {
  console.log(JSON.stringify(await validate(path, flag === '--after'), null, 2))
}
