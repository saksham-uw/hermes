#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REGION="${AWS_REGION:-us-east-1}"
FUNCTION_NAME="${HERMES_ACTIONS_FUNCTION:-hermes-actions}"
ROLE_NAME="${HERMES_ACTIONS_ROLE:-hermes-actions-lambda}"
ENV_FILE="$ROOT/.env"
OUT_DIR="$(mktemp -d)"
trap 'rm -rf "$OUT_DIR"' EXIT

if [[ ! -f "$ENV_FILE" ]]; then
  echo "missing $ENV_FILE" >&2
  exit 1
fi

set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a

for required in HERMES_ACTIONS_DEVICE_TOKEN HERMES_GITHUB_TOKEN HERMES_SLACK_USER_TOKEN HERMES_SLACK_DESTINATION; do
  if [[ -z "${!required:-}" ]]; then
    echo "$required is missing from .env" >&2
    exit 1
  fi
done

npm run actions:build --prefix "$ROOT"

ZIP="$OUT_DIR/function.zip"
LAMBDA_PKG="$OUT_DIR/package.json"
cat > "$LAMBDA_PKG" <<'JSON'
{"name":"hermes-actions","type":"module"}
JSON
(
  cd "$ROOT/packages/actions-service/dist"
  cp "$LAMBDA_PKG" ./package.json
  zip -q "$ZIP" ./*.js package.json
  rm -f ./package.json
)

ACCOUNT_ID="$(aws sts get-caller-identity --query Account --output text)"
ROLE_ARN="arn:aws:iam::${ACCOUNT_ID}:role/${ROLE_NAME}"

if ! aws iam get-role --role-name "$ROLE_NAME" >/dev/null 2>&1; then
  aws iam create-role --role-name "$ROLE_NAME" --assume-role-policy-document '{
    "Version": "2012-10-17",
    "Statement": [{
      "Effect": "Allow",
      "Principal": { "Service": "lambda.amazonaws.com" },
      "Action": "sts:AssumeRole"
    }]
  }' >/dev/null
  aws iam attach-role-policy \
    --role-name "$ROLE_NAME" \
    --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
  echo "waiting for IAM role to propagate"
  sleep 10
fi

ENV_JSON="$OUT_DIR/env.json"
python3 -c '
import json, os
from pathlib import Path
import sys
Path(sys.argv[1]).write_text(json.dumps({
    "Variables": {
        "HERMES_ACTIONS_DEVICE_TOKEN": os.environ["HERMES_ACTIONS_DEVICE_TOKEN"],
        "HERMES_GITHUB_TOKEN": os.environ["HERMES_GITHUB_TOKEN"],
        "HERMES_SLACK_USER_TOKEN": os.environ["HERMES_SLACK_USER_TOKEN"],
        "HERMES_SLACK_DESTINATION": os.environ["HERMES_SLACK_DESTINATION"],
        "HERMES_ACTIONS_IDEMPOTENCY_FILE": "/tmp/hermes-actions-idempotency.json",
    }
}))
' "$ENV_JSON"

if aws lambda get-function --function-name "$FUNCTION_NAME" --region "$REGION" >/dev/null 2>&1; then
  aws lambda update-function-code \
    --function-name "$FUNCTION_NAME" \
    --zip-file "fileb://$ZIP" \
    --region "$REGION" >/dev/null
  aws lambda wait function-updated --function-name "$FUNCTION_NAME" --region "$REGION"
  aws lambda update-function-configuration \
    --function-name "$FUNCTION_NAME" \
    --runtime nodejs22.x \
    --handler lambda.handler \
    --timeout 30 \
    --memory-size 256 \
    --environment "file://$ENV_JSON" \
    --region "$REGION" >/dev/null
  aws lambda wait function-updated --function-name "$FUNCTION_NAME" --region "$REGION"
else
  aws lambda create-function \
    --function-name "$FUNCTION_NAME" \
    --runtime nodejs22.x \
    --role "$ROLE_ARN" \
    --handler lambda.handler \
    --zip-file "fileb://$ZIP" \
    --timeout 30 \
    --memory-size 256 \
    --environment "file://$ENV_JSON" \
    --region "$REGION" >/dev/null
  aws lambda wait function-active --function-name "$FUNCTION_NAME" --region "$REGION"
fi

if ! aws lambda get-function-url-config --function-name "$FUNCTION_NAME" --region "$REGION" >/dev/null 2>&1; then
  aws lambda create-function-url-config \
    --function-name "$FUNCTION_NAME" \
    --auth-type NONE \
    --region "$REGION" >/dev/null
fi

aws lambda add-permission \
  --function-name "$FUNCTION_NAME" \
  --statement-id FunctionURLAllowPublic \
  --action lambda:InvokeFunctionUrl \
  --principal "*" \
  --function-url-auth-type NONE \
  --region "$REGION" >/dev/null 2>&1 || true
aws lambda add-permission \
  --function-name "$FUNCTION_NAME" \
  --statement-id FunctionURLAllowInvoke \
  --action lambda:InvokeFunction \
  --principal "*" \
  --region "$REGION" >/dev/null 2>&1 || true

URL="$(aws lambda get-function-url-config \
  --function-name "$FUNCTION_NAME" \
  --region "$REGION" \
  --query FunctionUrl --output text)"
URL="${URL%/}"

python3 -c '
import os, sys
from pathlib import Path
path, url = sys.argv[1], sys.argv[2]
token = os.environ["HERMES_ACTIONS_DEVICE_TOKEN"]
def c_string(value: str) -> str:
    import json
    return json.dumps(value)
Path(path).write_text(
    "/* Generated. Do not commit. */\n"
    "#pragma once\n"
    f"#define HERMES_ACTIONS_URL {c_string(url)}\n"
    f"#define HERMES_ACTIONS_DEVICE_TOKEN {c_string(token)}\n"
)
' "$ROOT/firmware/main/hermes_actions_config.h" "$URL"

echo "function_url=$URL"
