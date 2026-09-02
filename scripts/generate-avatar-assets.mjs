#!/usr/bin/env node

import { readFile, writeFile } from 'node:fs/promises'
import process from 'node:process'

import {
  advanceAvatarPlayback,
  createAvatarPlaybackState,
  expressionFromDefinition,
  playAvatarAnimation,
  renderAvatarDefinition,
  renderAvatarFrame,
  validateAvatarDefinition,
} from '@bible-strong/avatar-core'
import sharp from 'sharp'

const definitionPath = process.argv[2] ?? 'assets/avatar/kirby.avatar.json'
const headerPath = process.argv[3] ?? 'firmware/main/hermes_avatar_assets.h'
const sourcePath = process.argv[4] ?? 'firmware/main/hermes_avatar_assets.c'
const statesPath = process.argv[5] ?? 'assets/avatar/hermes-states.json'

const WIDTH = 148
const HEIGHT = 124
const SUPER_WIDTH = WIDTH * 8
const SUPER_HEIGHT = HEIGHT * 8
const TRANSITION_SAMPLES = 11
const rawDefinition = JSON.parse(await readFile(definitionPath, 'utf8'))
const stateDesign = JSON.parse(await readFile(statesPath, 'utf8'))
const validated = validateAvatarDefinition(rawDefinition)
if (!validated.ok) {
  throw new Error(validated.errors.map(error => `${error.path}: ${error.message}`).join('\n'))
}
const definition = validated.value
const expressionIndex = new Map(definition.expressionOrder.map((key, index) => [key, index]))
const transitionKinds = { spring: 0, smooth: 1, snappy: 2 }
const playbackModes = { loop: 0, once: 1, pingPong: 2 }
const stateNames = ['idle', 'rest', 'busy', 'thinking']

if (!Number.isInteger(stateDesign.restAfterMs) || stateDesign.restAfterMs < 1000) {
  throw new Error(`${statesPath}: restAfterMs must be an integer of at least 1000`)
}
for (const state of stateNames) {
  const routines = stateDesign.states?.[state]
  if (!Array.isArray(routines) || routines.length < 3 || routines.length > 5) {
    throw new Error(`${statesPath}: ${state} must define 3-5 routines`)
  }
  for (const routine of routines) {
    if (!definition.animations[routine]) {
      throw new Error(`${statesPath}: ${state} references unknown animation '${routine}'`)
    }
  }
}

const escapeXml = value =>
  value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('"', '&quot;')

const svgFor = (scene, layer) => {
  const paths = scene.geometry
  const bodyPaths = [...paths.backPaths, paths.headPath, ...paths.frontPaths]
  const body = bodyPaths.map(value => `<path d="${escapeXml(value)}" fill="#fff"/>`).join('')
  const eyes = `<defs><clipPath id="head"><path d="${escapeXml(paths.headPath)}"/></clipPath></defs><g clip-path="url(#head)"><path d="${escapeXml(paths.leftPath)}" fill="#fff" opacity="${paths.leftVisible ? 1 : 0}"/><path d="${escapeXml(paths.rightPath)}" fill="#fff" opacity="${paths.rightVisible ? 1 : 0}"/></g>`
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${SUPER_WIDTH}" height="${SUPER_HEIGHT}" viewBox="-180 -150 360 300">${layer === 'body' ? body : eyes}</svg>`
}

const alphaMask = async (scene, layer) => {
  const pixels = await sharp(Buffer.from(svgFor(scene, layer)))
    .resize(WIDTH, HEIGHT, { kernel: sharp.kernel.lanczos3 })
    .ensureAlpha()
    .raw()
    .toBuffer()
  const result = new Uint8Array(WIDTH * HEIGHT)
  for (let index = 0; index < result.length; index += 1) result[index] = pixels[index * 4 + 3]
  return result
}

const packScene = async scene => {
  const [body, eyes] = await Promise.all([alphaMask(scene, 'body'), alphaMask(scene, 'eyes')])
  const packed = new Uint16Array(WIDTH * HEIGHT)
  for (let index = 0; index < packed.length; index += 1) {
    packed[index] = body[index] | (eyes[index] << 8)
  }
  return packed
}

// PackBits-style compression over 16-bit body/eye coverage words.
const compress = input => {
  const output = []
  let cursor = 0
  while (cursor < input.length) {
    let run = 1
    while (cursor + run < input.length && input[cursor + run] === input[cursor] && run < 128) run++
    if (run >= 3) {
      output.push(0x80 | (run - 1), input[cursor] & 0xff, input[cursor] >> 8)
      cursor += run
      continue
    }
    const start = cursor
    cursor += run
    while (cursor < input.length && cursor - start < 128) {
      run = 1
      while (cursor + run < input.length && input[cursor + run] === input[cursor] && run < 128) run++
      if (run >= 3) break
      cursor += run
    }
    const length = cursor - start
    output.push(length - 1)
    for (let index = start; index < cursor; index += 1) {
      output.push(input[index] & 0xff, input[index] >> 8)
    }
  }
  return Uint8Array.from(output)
}

const verifyCompressedFrame = (encoded, expected) => {
  const decoded = []
  let cursor = 0
  while (cursor < encoded.length) {
    const command = encoded[cursor++]
    const run = (command & 0x80) !== 0
    const length = (command & 0x7f) + 1
    if (run) {
      const value = encoded[cursor] | (encoded[cursor + 1] << 8)
      cursor += 2
      for (let index = 0; index < length; index += 1) decoded.push(value)
    } else {
      for (let index = 0; index < length; index += 1) {
        decoded.push(encoded[cursor] | (encoded[cursor + 1] << 8))
        cursor += 2
      }
    }
  }
  if (decoded.length !== expected.length || decoded.some((value, index) => value !== expected[index])) {
    throw new Error('Internal avatar frame compression verification failed')
  }
}

const frames = []
const addFrame = async scene => {
  const packed = await packScene(scene)
  const compressed = compress(packed)
  verifyCompressedFrame(compressed, packed)
  frames.push(compressed)
  return frames.length - 1
}

for (const key of definition.expressionOrder) await addFrame(renderAvatarDefinition(definition, key))

const transitionMap = new Map()
const transitionRecords = []

const resolvedColors = expression => ({
  body: expression.colors?.body ?? definition.colors.body,
  eyes: expression.colors?.eyes ?? definition.colors.eyes,
})

const addTransition = async (sourceKey, targetKey, transition, durationMs) => {
  const mapKey = `${sourceKey}|${targetKey}|${transition}`
  if (transitionMap.has(mapKey) || sourceKey === targetKey) return

  const sourceDefinition = definition.expressions[sourceKey]
  const targetDefinition = definition.expressions[targetKey]
  if (!sourceDefinition || !targetDefinition) {
    throw new Error(`Cannot render transition ${sourceKey} -> ${targetKey}`)
  }

  const firstFrame = frames.length
  const sourceFrame = {
    expression: expressionFromDefinition(sourceKey, sourceDefinition),
    colors: resolvedColors(sourceDefinition),
  }
  for (let sample = 1; sample <= TRANSITION_SAMPLES; sample += 1) {
    const now = (durationMs * sample) / (TRANSITION_SAMPLES + 1)
    const state = {
      ...createAvatarPlaybackState(),
      activeExpression: targetKey,
      status: 'playing',
      directTransition: {
        from: sourceFrame,
        startedAt: 0,
        durationMs,
        transition,
      },
    }
    await addFrame(renderAvatarFrame(definition, state, now, { random: () => 0.5 }))
  }
  const record = {
    from: expressionIndex.get(sourceKey),
    to: expressionIndex.get(targetKey),
    kind: transitionKinds[transition],
    firstFrame,
  }
  transitionMap.set(mapKey, transitionRecords.length)
  transitionRecords.push(record)
}

for (const animationKey of definition.animationOrder) {
  const animation = definition.animations[animationKey]
  for (let targetIndex = 0; targetIndex < animation.steps.length; targetIndex += 1) {
    const targetStep = animation.steps[targetIndex]
    const sourceIndex = targetIndex === 0 ? animation.steps.length - 1 : targetIndex - 1
    const sourceKey = animation.steps[sourceIndex].expression
    const targetKey = targetStep.expression
    await addTransition(sourceKey, targetKey, targetStep.transition, targetStep.transitionMs)
  }
}

// State changes can happen while any step is being held. Pre-render every
// source expression used by the state system into every state routine's first
// expression, using Avatar Core's live direct-transition API.
const stateRoutineKeys = [...new Set(stateNames.flatMap(state => stateDesign.states[state]))]
const stateSourceExpressions = new Set(['neutral'])
for (const routineKey of [...stateRoutineKeys, 'waking']) {
  for (const step of definition.animations[routineKey].steps) {
    stateSourceExpressions.add(step.expression)
  }
}
const stateTargets = [...stateRoutineKeys, 'waking'].map(routineKey => {
  const firstStep = definition.animations[routineKey].steps[0]
  return {
    expression: firstStep.expression,
    transition: firstStep.transition,
    transitionMs: firstStep.transitionMs,
  }
})
for (const sourceKey of stateSourceExpressions) {
  for (const target of stateTargets) {
    await addTransition(
      sourceKey,
      target.expression,
      target.transition,
      target.transitionMs
    )
  }
}

// Exercise the public playback state machine against every exported preset.
for (const animationKey of definition.animationOrder) {
  const started = playAvatarAnimation(definition, animationKey, 0)
  if (!started.ok) throw new Error(started.error.message)
  const advanced = advanceAvatarPlayback(definition, started.value, 250, { random: () => 0.5 })
  renderAvatarFrame(definition, advanced, 250, { random: () => 0.5 })
}

const offsets = [0]
const stream = []
for (const frame of frames) {
  stream.push(...frame)
  offsets.push(stream.length)
}

const flatSteps = []
const animations = definition.animationOrder.map(key => {
  const animation = definition.animations[key]
  const firstStep = flatSteps.length
  for (let index = 0; index < animation.steps.length; index += 1) {
    const step = animation.steps[index]
    const previous = animation.steps[index === 0 ? animation.steps.length - 1 : index - 1]
    const transitionKey = `${previous.expression}|${step.expression}|${step.transition}`
    flatSteps.push({
      expression: expressionIndex.get(step.expression),
      holdMs: step.holdMs,
      transitionMs: step.transitionMs,
      transition: transitionKinds[step.transition],
      transitionAsset:
        previous.expression === step.expression ? 0xff : (transitionMap.get(transitionKey) ?? 0xff),
    })
  }
  return {
    key,
    firstStep,
    stepCount: animation.steps.length,
    playbackMode: playbackModes[animation.playbackMode],
    blink: animation.blink,
  }
})
const animationIndex = new Map(definition.animationOrder.map((key, index) => [key, index]))
const stateRoutines = []
const states = stateNames.map(name => {
  const firstRoutine = stateRoutines.length
  for (const routine of stateDesign.states[name]) stateRoutines.push(animationIndex.get(routine))
  return { name, firstRoutine, routineCount: stateDesign.states[name].length }
})

const cBytes = values => {
  const lines = []
  for (let index = 0; index < values.length; index += 16) {
    lines.push(`  ${values.slice(index, index + 16).map(value => `0x${value.toString(16).padStart(2, '0')}`).join(', ')},`)
  }
  return lines.join('\n')
}
const cU32 = values => {
  const lines = []
  for (let index = 0; index < values.length; index += 8) {
    lines.push(`  ${values.slice(index, index + 8).map(value => `${value}U`).join(', ')},`)
  }
  return lines.join('\n')
}
const cString = value => JSON.stringify(value)

const header = `// Generated by scripts/generate-avatar-assets.mjs from ${definitionPath}.\n#pragma once\n\n#include <stdint.h>\n\n#define HERMES_AVATAR_ASSET_WIDTH ${WIDTH}\n#define HERMES_AVATAR_ASSET_HEIGHT ${HEIGHT}\n#define HERMES_AVATAR_EXPRESSION_COUNT ${definition.expressionOrder.length}\n#define HERMES_AVATAR_TRANSITION_SAMPLES ${TRANSITION_SAMPLES}\n#define HERMES_AVATAR_FRAME_COUNT ${frames.length}\n#define HERMES_AVATAR_TRANSITION_COUNT ${transitionRecords.length}\n#define HERMES_AVATAR_ANIMATION_COUNT ${animations.length}\n#define HERMES_AVATAR_STEP_COUNT ${flatSteps.length}\n#define HERMES_AVATAR_STATE_COUNT ${states.length}\n#define HERMES_AVATAR_STATE_ROUTINE_COUNT ${stateRoutines.length}\n#define HERMES_AVATAR_REST_AFTER_MS ${stateDesign.restAfterMs}U\n\ntypedef enum {\n  HERMES_AVATAR_SPRING = 0,\n  HERMES_AVATAR_SMOOTH = 1,\n  HERMES_AVATAR_SNAPPY = 2,\n} hermes_avatar_transition_kind_t;\n\ntypedef struct {\n  uint8_t expression;\n  uint16_t hold_ms;\n  uint16_t transition_ms;\n  uint8_t transition;\n  uint8_t transition_asset;\n} hermes_avatar_asset_step_t;\n\ntypedef struct {\n  const char *key;\n  uint8_t first_step;\n  uint8_t step_count;\n  uint8_t playback_mode;\n  uint8_t blink_enabled;\n  uint16_t blink_initial_delay_ms;\n  uint16_t blink_min_interval_ms;\n  uint16_t blink_max_interval_ms;\n  uint16_t blink_duration_ms;\n} hermes_avatar_asset_animation_t;\n\ntypedef struct {\n  uint8_t from_expression;\n  uint8_t to_expression;\n  uint8_t kind;\n  uint16_t first_frame;\n} hermes_avatar_asset_transition_t;\n\ntypedef struct {\n  const char *name;\n  uint8_t first_routine;\n  uint8_t routine_count;\n} hermes_avatar_asset_state_t;\n\nextern const char *const g_hermes_avatar_expression_keys[HERMES_AVATAR_EXPRESSION_COUNT];\nextern const hermes_avatar_asset_step_t g_hermes_avatar_steps[HERMES_AVATAR_STEP_COUNT];\nextern const hermes_avatar_asset_animation_t g_hermes_avatar_animations[HERMES_AVATAR_ANIMATION_COUNT];\nextern const hermes_avatar_asset_transition_t g_hermes_avatar_transitions[HERMES_AVATAR_TRANSITION_COUNT];\nextern const hermes_avatar_asset_state_t g_hermes_avatar_states[HERMES_AVATAR_STATE_COUNT];\nextern const uint8_t g_hermes_avatar_state_routines[HERMES_AVATAR_STATE_ROUTINE_COUNT];\nextern const uint32_t g_hermes_avatar_frame_offsets[HERMES_AVATAR_FRAME_COUNT + 1];\nextern const uint8_t g_hermes_avatar_frame_data[${stream.length}];\n`

let source = `// Generated by scripts/generate-avatar-assets.mjs. Do not edit.\n#include "hermes_avatar_assets.h"\n\nconst char *const g_hermes_avatar_expression_keys[HERMES_AVATAR_EXPRESSION_COUNT] = {\n${definition.expressionOrder.map(key => `  ${cString(key)},`).join('\n')}\n};\n\nconst hermes_avatar_asset_step_t g_hermes_avatar_steps[HERMES_AVATAR_STEP_COUNT] = {\n${flatSteps.map(step => `  {${step.expression}, ${step.holdMs}, ${step.transitionMs}, ${step.transition}, ${step.transitionAsset}},`).join('\n')}\n};\n\nconst hermes_avatar_asset_animation_t g_hermes_avatar_animations[HERMES_AVATAR_ANIMATION_COUNT] = {\n${animations.map(animation => `  {${cString(animation.key)}, ${animation.firstStep}, ${animation.stepCount}, ${animation.playbackMode}, ${animation.blink.enabled ? 1 : 0}, ${animation.blink.initialDelayMs}, ${animation.blink.minIntervalMs}, ${animation.blink.maxIntervalMs}, ${animation.blink.durationMs}},`).join('\n')}\n};\n\nconst hermes_avatar_asset_transition_t g_hermes_avatar_transitions[HERMES_AVATAR_TRANSITION_COUNT] = {\n${transitionRecords.map(record => `  {${record.from}, ${record.to}, ${record.kind}, ${record.firstFrame}},`).join('\n')}\n};\n\nconst uint32_t g_hermes_avatar_frame_offsets[HERMES_AVATAR_FRAME_COUNT + 1] = {\n${cU32(offsets)}\n};\n\nconst uint8_t g_hermes_avatar_frame_data[${stream.length}] = {\n${cBytes(stream)}\n};\n`

source = source.replace(
  '\nconst uint32_t g_hermes_avatar_frame_offsets',
  `\nconst hermes_avatar_asset_state_t g_hermes_avatar_states[HERMES_AVATAR_STATE_COUNT] = {\n${states.map(state => `  {${cString(state.name)}, ${state.firstRoutine}, ${state.routineCount}},`).join('\n')}\n};\n\nconst uint8_t g_hermes_avatar_state_routines[HERMES_AVATAR_STATE_ROUTINE_COUNT] = {\n  ${stateRoutines.join(', ')},\n};\n\nconst uint32_t g_hermes_avatar_frame_offsets`
)

await Promise.all([writeFile(headerPath, header), writeFile(sourcePath, source)])
const rawBytes = frames.length * WIDTH * HEIGHT * 2
console.log(
  `Avatar SDK assets: ${frames.length} frames (${transitionRecords.length} exact transitions), ` +
    `${stream.length} compressed bytes / ${rawBytes} raw bytes ` +
    `(${((stream.length / rawBytes) * 100).toFixed(1)}%)`
)
