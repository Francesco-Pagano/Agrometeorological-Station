import time
import json
import paho.mqtt.client as mqtt

def main(dati, ACCESS_TOKEN, THINGSBOARD_HOST, THINGSBOARD_MQTT_PORT, KEEP_ALIVE):
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    client.username_pw_set(ACCESS_TOKEN)
    client.connect(THINGSBOARD_HOST, THINGSBOARD_MQTT_PORT, KEEP_ALIVE)
    client.loop_start()
    try:
        time.sleep(2)
        response = client.publish('v1/devices/me/telemetry', json.dumps(dati), 1)
        response.wait_for_publish()
        if response.is_published():
            status="Published"
    except :
        status="Error"
    client.loop_stop()
    client.disconnect()
    return status
