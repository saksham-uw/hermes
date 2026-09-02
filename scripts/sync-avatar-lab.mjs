#!/usr/bin/env node

import { readFile, mkdir, writeFile } from 'node:fs/promises'
import path from 'node:path'
import process from 'node:process'

import { validateAvatarDefinition } from '@bible-strong/avatar-core'

const upstreamPath = process.argv[2]
const outputPath = process.argv[3] ?? 'assets/avatar/kirby.avatar.json'

if (!upstreamPath) {
  throw new Error(
    'Usage: npm run avatar:sync -- /path/to/avatar-lab/src/features/studio/defaultStudioDocument.json'
  )
}

const document = JSON.parse(await readFile(upstreamPath, 'utf8'))
const avatar = document.library?.avatars?.find(candidate => candidate.name === 'Kirby')
if (!avatar) throw new Error(`Kirby was not found in ${upstreamPath}`)

const neutralDefaults = {
  widthLeft: 20,
  widthRight: 20,
  heightLeft: 50,
  heightRight: 50,
  spacing: 35,
  positionXLeft: 0,
  positionXRight: 0,
  positionYLeft: -7,
  positionYRight: -7,
  leftAngle: 0,
  rightAngle: 0,
}

const neutral = {
  id: 'expression-neutral',
  semanticKey: 'neutral',
  headX: 0,
  headY: 0,
  headZ: 0,
  ...avatar.eyes,
  perspective: 1,
  eyeMotion: 'none',
  bodyMotion: 'none',
}

const applyEyeDefaults = expression => {
  const result = { ...expression }
  for (const field of Object.keys(neutralDefaults)) {
    result[field] = expression[field] + avatar.eyes[field] - neutralDefaults[field]
  }
  result.widthLeft = Math.max(10, result.widthLeft)
  result.widthRight = Math.max(10, result.widthRight)
  result.heightLeft = Math.max(10, result.heightLeft)
  result.heightRight = Math.max(10, result.heightRight)
  return result
}

const expressions = [neutral, ...document.expressions.map(applyEyeDefaults)]
const expressionKeyById = new Map(expressions.map(item => [item.id, item.semanticKey]))

const mapExpression = expression => ({
  head: { x: expression.headX, y: expression.headY, z: expression.headZ },
  eyes: {
    left: {
      width: expression.widthLeft,
      height: expression.heightLeft,
      x: expression.positionXLeft,
      y: expression.positionYLeft,
      angle: expression.leftAngle,
    },
    right: {
      width: expression.widthRight,
      height: expression.heightRight,
      x: expression.positionXRight,
      y: expression.positionYRight,
      angle: expression.rightAngle,
    },
    spacing: expression.spacing,
  },
  perspective: expression.perspective,
  motion: { eyes: expression.eyeMotion, body: expression.bodyMotion },
  ...(expression.bodyColor || expression.eyeColor
    ? {
        colors: {
          ...(expression.bodyColor ? { body: expression.bodyColor } : {}),
          ...(expression.eyeColor ? { eyes: expression.eyeColor } : {}),
        },
      }
    : {}),
})

const definition = {
  schema: 'bible-strong/avatar-definition',
  schemaVersion: 1,
  name: avatar.name,
  body: {
    primary: { ...avatar.body.primary },
    nodes: avatar.body.nodes.map(node => ({
      surface: { ...node.surface },
      position: [...node.position],
      rotation: [...node.rotation],
    })),
  },
  colors: { ...avatar.colors },
  expressions: Object.fromEntries(
    expressions.map(expression => [expression.semanticKey, mapExpression(expression)])
  ),
  expressionOrder: expressions.map(expression => expression.semanticKey),
  animations: Object.fromEntries(
    document.sequences.map(sequence => [
      sequence.semanticKey,
      {
        playbackMode: sequence.playbackMode,
        steps: sequence.steps.map(step => ({
          expression: expressionKeyById.get(step.expressionId),
          holdMs: step.holdMs,
          transitionMs: step.transitionMs,
          transition: step.transition,
        })),
        blink: { ...sequence.blink },
        metadata: {
          label: sequence.name,
          description: sequence.description,
          group: sequence.group,
        },
      },
    ])
  ),
  animationOrder: document.sequences.map(sequence => sequence.semanticKey),
}

const validated = validateAvatarDefinition(definition)
if (!validated.ok) {
  throw new Error(
    `Avatar Lab export did not validate:\n${validated.errors
      .map(error => `${error.path || '/'}: ${error.message}`)
      .join('\n')}`
  )
}

await mkdir(path.dirname(outputPath), { recursive: true })
await writeFile(outputPath, `${JSON.stringify(validated.value, null, 2)}\n`)
console.log(
  `Synced ${validated.value.name}: ${validated.value.expressionOrder.length} expressions, ` +
    `${validated.value.animationOrder.length} animations -> ${outputPath}`
)
