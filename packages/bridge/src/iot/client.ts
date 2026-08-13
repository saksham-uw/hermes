import { mqtt, iot, io } from "aws-iot-device-sdk-v2";
import { fromNodeProviderChain } from "@aws-sdk/credential-providers";
import type { DeviceDownMessage } from "@hermes/protocol";
import { topics } from "@hermes/protocol";
import type { BridgeConfig } from "../config.js";

export type DownPublisher = (msg: DeviceDownMessage) => Promise<void>;

export type IotBus = {
  publishDown: DownPublisher;
  onUp: (handler: (raw: string) => void) => void;
  close: () => Promise<void>;
};

/**
 * Connect to AWS IoT Core over MQTT (WebSocket + SigV4 / IAM).
 */
export async function connectIot(config: BridgeConfig): Promise<IotBus> {
  const t = topics(config.userId);
  const credProvider = fromNodeProviderChain({
    clientConfig: { region: config.awsRegion },
  });
  const credentials = await credProvider();

  const builder = iot.AwsIotMqttConnectionConfigBuilder.new_with_websockets()
    .with_clean_session(true)
    .with_client_id(config.bridgeClientId)
    .with_endpoint(config.iotEndpoint)
    .with_keep_alive_seconds(30)
    .with_credentials(
      config.awsRegion,
      credentials.accessKeyId,
      credentials.secretAccessKey,
      credentials.sessionToken,
    );

  const cfg = builder.build();
  const client = new mqtt.MqttClient(new io.ClientBootstrap());
  const connection = client.new_connection(cfg);

  const upHandlers: Array<(raw: string) => void> = [];

  await connection.connect();
  console.log(`[iot] connected as ${config.bridgeClientId}`);

  await connection.subscribe(
    t.deviceUp,
    mqtt.QoS.AtLeastOnce,
    (_topic, payload) => {
      const raw = Buffer.from(payload).toString("utf8");
      for (const h of upHandlers) {
        try {
          h(raw);
        } catch (err) {
          console.error("[iot] up handler error", err);
        }
      }
    },
  );
  console.log(`[iot] subscribed ${t.deviceUp}`);

  return {
    async publishDown(msg) {
      const body = JSON.stringify(msg);
      await connection.publish(t.deviceDown, body, mqtt.QoS.AtLeastOnce);
    },
    onUp(handler) {
      upHandlers.push(handler);
    },
    async close() {
      try {
        await connection.disconnect();
      } catch {
        /* ignore */
      }
    },
  };
}
