// MQTT-over-WebSocket config — copy to config.js and fill in the real password.
window.MQTT_CONFIG = {
  wssUrl:   'wss://b14f23266c4e43fa94f6a6dbe430a163.s1.eu.hivemq.cloud:8884/mqtt',
  cmdTopic: 'cmd/antitheft/commands',
  pubUser:  'dashboard-pub',
  pubPass:  '<PASTE_HIVEMQ_DASHBOARD_PUB_PASSWORD_HERE>'
};
