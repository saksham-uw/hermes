#!/usr/bin/env bash
# Provision AWS IoT Core resources for Hermes (Thing, cert, policies, IAM).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
REGION="${AWS_REGION:-us-east-1}"
USER_ID="${HERMES_USER_ID:-saksham}"
THING_NAME="${HERMES_THING_NAME:-hermes-device}"
POLICY_NAME="${HERMES_IOT_POLICY:-HermesDevicePolicy}"
BRIDGE_POLICY_NAME="${HERMES_BRIDGE_IAM_POLICY:-HermesBridgeIoT}"
ACCOUNT_ID="$(aws sts get-caller-identity --query Account --output text)"
OUT_DIR="${ROOT}/deploy/iot/out"
FIRMWARE_CERTS="${ROOT}/firmware/certs"

mkdir -p "$OUT_DIR" "$FIRMWARE_CERTS"

echo "==> Region=$REGION Account=$ACCOUNT_ID Thing=$THING_NAME User=$USER_ID"

ENDPOINT="$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --region "$REGION" --query endpointAddress --output text)"
echo "$ENDPOINT" > "$OUT_DIR/endpoint.txt"
echo "==> IoT endpoint: $ENDPOINT"

# --- Thing ---
if aws iot describe-thing --thing-name "$THING_NAME" --region "$REGION" >/dev/null 2>&1; then
  echo "==> Thing exists: $THING_NAME"
else
  aws iot create-thing --thing-name "$THING_NAME" --region "$REGION" >/dev/null
  echo "==> Created Thing: $THING_NAME"
fi

# --- Device IoT policy ---
DEVICE_POLICY_DOC=$(cat <<EOF
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:${REGION}:${ACCOUNT_ID}:client/\${iot:Connection.Thing.ThingName}*"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": "arn:aws:iot:${REGION}:${ACCOUNT_ID}:topic/hermes/${USER_ID}/device/up"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": "arn:aws:iot:${REGION}:${ACCOUNT_ID}:topicfilter/hermes/${USER_ID}/device/down"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Receive",
      "Resource": "arn:aws:iot:${REGION}:${ACCOUNT_ID}:topic/hermes/${USER_ID}/device/down"
    }
  ]
}
EOF
)

if aws iot get-policy --policy-name "$POLICY_NAME" --region "$REGION" >/dev/null 2>&1; then
  aws iot create-policy-version \
    --policy-name "$POLICY_NAME" \
    --policy-document "$DEVICE_POLICY_DOC" \
    --set-as-default \
    --region "$REGION" >/dev/null
  echo "==> Updated IoT policy: $POLICY_NAME"
else
  aws iot create-policy \
    --policy-name "$POLICY_NAME" \
    --policy-document "$DEVICE_POLICY_DOC" \
    --region "$REGION" >/dev/null
  echo "==> Created IoT policy: $POLICY_NAME"
fi

# --- Device certificate (create once; reuse if marker present) ---
CERT_ID_FILE="$OUT_DIR/certificateId.txt"
if [[ -f "$CERT_ID_FILE" && -f "$FIRMWARE_CERTS/device.cert.pem" && -f "$FIRMWARE_CERTS/device.private.key" ]]; then
  CERT_ID="$(cat "$CERT_ID_FILE")"
  echo "==> Reusing existing device cert: $CERT_ID"
else
  echo "==> Creating device certificate + keys"
  CREATE_OUT="$(aws iot create-keys-and-certificate \
    --set-as-active \
    --certificate-pem-outfile "$FIRMWARE_CERTS/device.cert.pem" \
    --public-key-outfile "$FIRMWARE_CERTS/device.public.key" \
    --private-key-outfile "$FIRMWARE_CERTS/device.private.key" \
    --region "$REGION")"
  CERT_ARN="$(echo "$CREATE_OUT" | python3 -c 'import sys,json; print(json.load(sys.stdin)["certificateArn"])')"
  CERT_ID="$(echo "$CREATE_OUT" | python3 -c 'import sys,json; print(json.load(sys.stdin)["certificateId"])')"
  echo "$CERT_ID" > "$CERT_ID_FILE"
  echo "$CERT_ARN" > "$OUT_DIR/certificateArn.txt"
  chmod 600 "$FIRMWARE_CERTS/device.private.key"
fi

CERT_ARN="$(cat "$OUT_DIR/certificateArn.txt" 2>/dev/null || aws iot describe-certificate --certificate-id "$(cat "$CERT_ID_FILE")" --region "$REGION" --query certificateDescription.certificateArn --output text)"
echo "$CERT_ARN" > "$OUT_DIR/certificateArn.txt"

aws iot attach-policy --policy-name "$POLICY_NAME" --target "$CERT_ARN" --region "$REGION" 2>/dev/null || true
aws iot attach-thing-principal --thing-name "$THING_NAME" --principal "$CERT_ARN" --region "$REGION" 2>/dev/null || true
echo "==> Attached policy + Thing to certificate"

# Amazon Root CA 1
if [[ ! -f "$FIRMWARE_CERTS/AmazonRootCA1.pem" ]]; then
  curl -fsSL "https://www.amazontrust.com/repository/AmazonRootCA1.pem" -o "$FIRMWARE_CERTS/AmazonRootCA1.pem"
  echo "==> Downloaded AmazonRootCA1.pem"
fi

# --- Bridge IAM managed policy ---
BRIDGE_POLICY_DOC=$(cat <<EOF
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "HermesIoTConnect",
      "Effect": "Allow",
      "Action": ["iot:Connect"],
      "Resource": [
        "arn:aws:iot:${REGION}:${ACCOUNT_ID}:client/hermes-bridge-*"
      ]
    },
    {
      "Sid": "HermesIoTPubSub",
      "Effect": "Allow",
      "Action": ["iot:Publish", "iot:Receive"],
      "Resource": [
        "arn:aws:iot:${REGION}:${ACCOUNT_ID}:topic/hermes/${USER_ID}/*"
      ]
    },
    {
      "Sid": "HermesIoTSubscribe",
      "Effect": "Allow",
      "Action": ["iot:Subscribe"],
      "Resource": [
        "arn:aws:iot:${REGION}:${ACCOUNT_ID}:topicfilter/hermes/${USER_ID}/*"
      ]
    }
  ]
}
EOF
)

POLICY_ARN="arn:aws:iam::${ACCOUNT_ID}:policy/${BRIDGE_POLICY_NAME}"
if aws iam get-policy --policy-arn "$POLICY_ARN" >/dev/null 2>&1; then
  aws iam create-policy-version \
    --policy-arn "$POLICY_ARN" \
    --policy-document "$BRIDGE_POLICY_DOC" \
    --set-as-default >/dev/null
  echo "==> Updated IAM policy: $BRIDGE_POLICY_NAME"
else
  aws iam create-policy \
    --policy-name "$BRIDGE_POLICY_NAME" \
    --policy-document "$BRIDGE_POLICY_DOC" >/dev/null
  echo "==> Created IAM policy: $BRIDGE_POLICY_NAME"
fi

# Attach to current user (bootstrap) and optionally an EC2 instance role.
CALLER_ARN="$(aws sts get-caller-identity --query Arn --output text)"
if [[ "$CALLER_ARN" == arn:aws:iam::*:user/* ]]; then
  USER_NAME="${CALLER_ARN##*/}"
  aws iam attach-user-policy --user-name "$USER_NAME" --policy-arn "$POLICY_ARN" 2>/dev/null || true
  echo "==> Attached $BRIDGE_POLICY_NAME to IAM user $USER_NAME"
fi

if [[ -n "${HERMES_EC2_INSTANCE_PROFILE_ROLE:-}" ]]; then
  aws iam attach-role-policy \
    --role-name "$HERMES_EC2_INSTANCE_PROFILE_ROLE" \
    --policy-arn "$POLICY_ARN" 2>/dev/null || true
  echo "==> Attached $BRIDGE_POLICY_NAME to role $HERMES_EC2_INSTANCE_PROFILE_ROLE"
fi

# Write env snippet
cat > "$OUT_DIR/hermes.env" <<EOF
AWS_REGION=$REGION
HERMES_IOT_ENDPOINT=$ENDPOINT
HERMES_USER_ID=$USER_ID
HERMES_THING_NAME=$THING_NAME
HERMES_BRIDGE_CLIENT_ID=hermes-bridge-1
EOF

cp "$OUT_DIR/hermes.env" "$ROOT/.env" 2>/dev/null || true

cat <<EOF

Provision complete.

  Endpoint:     $ENDPOINT
  Thing:        $THING_NAME
  Device certs: $FIRMWARE_CERTS/
  Env snippet:  $OUT_DIR/hermes.env

Next:
  1. On EC2, attach IAM policy $BRIDGE_POLICY_NAME to the instance role
     (or set HERMES_EC2_INSTANCE_PROFILE_ROLE=<role> and re-run).
  2. Copy firmware/certs onto the device / flash with ESP-IDF.
  3. npm install && npm run build && npm run start:bridge

EOF
