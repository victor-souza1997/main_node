//************************************************************
// this is a simple example that uses the painlessMesh library to
// connect to a node on another network. Please see the WIKI on gitlab
// for more details
// https://gitlab.com/painlessMesh/painlessMesh/wikis/bridge-between-mesh-and-another-network
//************************************************************
#include "IPAddress.h"
#include "painlessMesh.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "PubSubClient.h"

#define MESH_PREFIX "whateverYouLike"
#define MESH_PASSWORD "somethingSneaky"
#define MESH_PORT 5555

#define STATION_SSID "AP_405"
#define STATION_PASSWORD "mercuryHg100"
#define STATION_PORT 5555
#define CHANNEL 6
#define HOSTNAME "MQTT_Bridge"
// uint8_t station_ip[4] = {192, 168, 1, 128}; // IP of the server

// mesh variavels
painlessMesh mesh;
WiFiClient wifiClient;

AsyncWebServer server(80);
IPAddress myIP(0, 0, 0, 0);
IPAddress myAPIP(0, 0, 0, 0);

// Function prototype
IPAddress getlocalIP();
void receivedCallback(uint32_t from, String &msg);
void newConnectionCallback(uint32_t nodeId);
void changedConnectionCallback();
void nodeTimeAdjustedCallback(int32_t offset);
void mqttCallback(char *topic, byte *payload, unsigned int length);
void reconnect();
// hivemq pubblic broker address and port
char mqttBroker[] = "broker.hivemq.com";
uint8_t initialized = 0;
uint32_t nexttime = 0;

// topic's suffix: everyone can publish/subscribe to this public broker,
// you have to change the following 2 defines
#define PUBPLISHSUFFIX "painlessMesh/from/"
#define SUBSCRIBESUFFIX "painlessMesh/to/"
#define MQTTPORT 1883

#define PUBPLISHFROMGATEWAYSUFFIX PUBPLISHSUFFIX "gateway"

#define CHECKCONNDELTA 60 // check interval ( seconds ) for mqtt connection

PubSubClient mqttClient;

void setup()
{
  Serial.begin(115200);
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION); // set before init() so that you can see startup messages

  // Channel set to 6. Make sure to use the same channel for your mesh and for you other
  // network (STATION_SSID)

  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA);
  // Setup over the air update support
  // mesh.initOTA("bridge");

  mesh.stationManual(STATION_SSID, STATION_PASSWORD);
  //  Bridge node, should (in most cases) be a root node. See [the wiki](https://gitlab.com/painlessMesh/painlessMesh/wikis/Possible-challenges-in-mesh-formation) for some background
  mesh.setRoot(true);
  // This node and all other nodes should ideally know the mesh contains a root, so call this on all nodes
  mesh.setContainsRoot(true);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  myAPIP = IPAddress(mesh.getAPIP());
  Serial.println("My AP IP is " + myAPIP.toString());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    request->send(200, "text/html", "<form>Text to Broadcast<br><input type='text' name='BROADCAST'><br><br><input type='submit' value='Submit'></form>");
    if (request->hasArg("BROADCAST")){
      String msg = request->arg("BROADCAST");
      mesh.sendBroadcast(msg);
    } });
  server.begin();
  mqttClient.setServer(mqttBroker, MQTTPORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setClient(wifiClient);
}

void loop()
{
  mesh.update();
  mqttClient.loop();

  if (myIP != getlocalIP())
  {
    myIP = getlocalIP();
    Serial.println("My IP is " + myIP.toString());
    initialized = 1;
  }
  if ((millis() >= nexttime) && (initialized))
  {
    nexttime = millis() + CHECKCONNDELTA * 1000;
    if (!mqttClient.connected())
    {
      reconnect();
    }
  }
}

IPAddress getlocalIP()
{
  return IPAddress(mesh.getStationIP());
}

// Needed for painless library
void receivedCallback(uint32_t from, String &msg)
{
  Serial.printf("bridge: Received from %u msg=%s\n", from, msg.c_str());
  String topic = PUBPLISHSUFFIX + String(from);
  mqttClient.publish(topic.c_str(), msg.c_str());
}

void newConnectionCallback(uint32_t nodeId)
{
  Serial.printf("--> startHere: New Connection, nodeId = %u\n", nodeId);
}

void changedConnectionCallback()
{
  Serial.printf("Changed connections\n");
}

void nodeTimeAdjustedCallback(int32_t offset)
{
  Serial.printf("Adjusted time %u. Offset = %d\n", mesh.getNodeTime(), offset);
}

// messages received from the mqtt broker
void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
  char *cleanPayload = (char *)malloc(length + 1);
  payload[length] = '\0';
  memcpy(cleanPayload, payload, length + 1);
  String msg = String(cleanPayload);
  free(cleanPayload);

  Serial.printf("mc t:%s  p:%s\n", topic, payload);

  String targetStr = String(topic).substring(strlen(SUBSCRIBESUFFIX));
  if (targetStr == "gateway")
  {
    if (msg == "getNodes")
    {
      auto nodes = mesh.getNodeList(true);
      String str;
      for (auto &&id : nodes)
        str += String(id) + String(" ");
      mqttClient.publish(PUBPLISHFROMGATEWAYSUFFIX, str.c_str());
    }
  }
  else if (targetStr == "broadcast")
  {
    mesh.sendBroadcast(msg);
  }
  else
  {
    uint32_t target = strtoul(targetStr.c_str(), NULL, 10);
    if (mesh.isConnected(target))
    {
      mesh.sendSingle(target, msg);
    }
    else
    {
      mqttClient.publish(PUBPLISHFROMGATEWAYSUFFIX, "Client not connected!");
    }
  }
}

void reconnect()
{
  // byte mac[6];
  char MAC[9];
  int i;

  // unique string
  // WiFi.macAddress(mac);
  // sprintf(MAC,"%02X",mac[2],mac[3],mac[4],mac[5]);
  sprintf(MAC, "%08X", (uint32_t)ESP.getEfuseMac()); // generate unique addresss.
  // Loop until we're reconnected
  while (!mqttClient.connected())
  {
    Serial.println("Attempting MQTT connection...");
    // Attemp to connect
    if (mqttClient.connect(/*MQTT_CLIENT_NAME*/ MAC))
    {
      Serial.println("Connected");
      mqttClient.publish(PUBPLISHFROMGATEWAYSUFFIX, "Ready!");
      mqttClient.subscribe(SUBSCRIBESUFFIX "#");
    }
    else
    {
      Serial.print("Failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 2 seconds");
      // Wait 2 seconds before retrying
      delay(2000);
      mesh.update();
      mqttClient.loop();
    }
  }
}