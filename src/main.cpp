//************************************************************
// this is a MqttBroker example that uses the painlessMesh library
//
// connect to a another network and relay messages from a MQTT broker to the nodes of the mesh network.
//
// - To send a message to a mesh node, you can publish it to "painlessMesh/to/NNNN" where NNNN equals the nodeId.
// - To broadcast a message to all nodes in the mesh you can publish it to "painlessMesh/to/broadcast".
// - When you publish "getNodes" to "painlessMesh/to/gateway" you receive the mesh topology as JSON
//
// - Every message from the mesh which is sent to the gateway node will be published to "painlessMesh/from/NNNN" where NNNN
//   is the nodeId from which the packet was sent.
//
// - The web server has only 3 pages:
//     ip_address_of_the_bridge      to broadcast messages to all nodes
//     ip_address_of_the_bridge/map  to show the topology of the network
//     ip_address_of_the_bridge/scan to get the topology of the network ( json format )
//
//************************************************************
//#include <Arduino.h>
#include "painlessMesh.h"
#include "PubSubClient.h"
#include "Button2.h"
#include <TFT_eSPI.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#ifdef ESP8266
#include "Hash.h"
#include <ESPAsyncTCP.h>
#else
#include <AsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>

// PainlessMesh credentials ( name, password and port ): You should change these
#define MESH_PREFIX "whateverYouLike"
#define MESH_PASSWORD "somethingSneaky"
#define MESH_PORT 5555

// WiFi credentials: should match your access point!
#define STATION_SSID "AP_405"
#define STATION_PASSWORD "mercuryHg100"
#define HOSTNAME "MQTT_Bridge"

const char *serverName = "https://us-east-1.aws.webhooks.mongodb-realm.com/api/client/v2.0/app/temperature_analysis-gjbaa/service/temperatura/incoming_webhook/api?secret=10minute";

Scheduler userScheduler; // to control your personal task

painlessMesh mesh;
WiFiClient wifiClient;
AsyncWebServer server(80);

// Prototypes
void receivedCallback(const uint32_t &from, const String &msg);

IPAddress getlocalIP();
IPAddress myIP(0, 0, 0, 0);
IPAddress myAPIP(0, 0, 0, 0);

#define ADC_EN 14 // ADC_EN is the ADC detection enable port
#define ADC_PIN 34
#define BUTTON_1 35
#define BUTTON_2 0
// TFT end
Button2 btn1(BUTTON_1);
Button2 btn2(BUTTON_2);

bool calc_delay = false;
SimpleList<uint32_t> nodes;
uint32_t nsent = 0;
char buff[512];
uint32_t nexttime = 0;
uint8_t initialized = 0;

StaticJsonDocument<500> doc; // json variable to send to database

// Task taskSendMessage( TASK_SECOND * 1 , TASK_FOREVER, &sendMessage );

void POSTData() // post data in DB
{

  if (WiFi.status() == WL_CONNECTED) // check wifi connection
  {
    HTTPClient http; // create http object

    http.begin(serverName);                             // start http object
    http.addHeader("Content-Type", "application/json"); // add header to the message
    String json;                                        // variable to print json content
    serializeJson(doc, json);                           // convert json into string
    Serial.println(json);                               // print string content
    int httpResponseCode = http.POST(json);             // post and get retuned message
    Serial.print("resposta:");
    Serial.println(httpResponseCode);
  }
}

// messages received from painless mesh network
void receivedCallback(const uint32_t &from, const String &msg)
{
  Serial.printf("bridge: Received from %u msg=%s\n", from, msg.c_str());
  doc["sensor"]["temperature"] = msg.c_str();
  doc["dispositivo"]["id"] = from;

  POSTData();
}
void newConnectionCallback(uint32_t nodeId)
{
  Serial.printf("--> Start: New Connection, nodeId = %u\n", nodeId);
  Serial.printf("--> Start: New Connection, %s\n", mesh.subConnectionJson(true).c_str());
}
void changedConnectionCallback()
{
  Serial.printf("Changed connections\n");

  nodes = mesh.getNodeList();
  Serial.printf("Num nodes: %d\n", nodes.size());
  Serial.printf("Connection list:");
  SimpleList<uint32_t>::iterator node = nodes.begin();
  while (node != nodes.end())
  {
    Serial.printf(" %u", *node);
    node++;
  }
  Serial.println();
  calc_delay = true;

  sprintf(buff, "Nodes:%d", nodes.size());
}
void nodeTimeAdjustedCallback(int32_t offset)
{
  Serial.printf("Adjusted time %u Offset = %d\n", mesh.getNodeTime(), offset);
}
void onNodeDelayReceived(uint32_t nodeId, int32_t delay)
{
  Serial.printf("Delay from node:%u delay = %d\n", nodeId, delay);
}

IPAddress getlocalIP() { return IPAddress(mesh.getStationIP()); }
String scanprocessor(const String &var)
{
  if (var == "SCAN")
    return mesh.subConnectionJson(false);
  return String();
}
void setup()
{
  Serial.begin(115200);

  // mesh.setDebugMsgTypes( ERROR | MESH_STATUS | CONNECTION | SYNC | COMMUNICATION | MSG_TYPES | REMOTE ); // all types on except GENERAL
  // mesh.setDebugMsgTypes( ERROR | MESH_STATUS | CONNECTION | SYNC | COMMUNICATION | GENERAL | MSG_TYPES | REMOTE ); // all types on
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION); // set before init() so that you can see startup messages

  // Channel set to 1. Make sure to use the same channel for your mesh and for you other
  // network (STATION_SSID)
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  mesh.stationManual(STATION_SSID, STATION_PASSWORD);
  mesh.setHostname(HOSTNAME);

  // Bridge node, should (in most cases) be a root node. See [the wiki](https://gitlab.com/painlessMesh/painlessMesh/wikis/Possible-challenges-in-mesh-formation) for some background
  mesh.setRoot(true);
  // This node and all other nodes should ideally know the mesh contains a root, so call this on all nodes
  mesh.setContainsRoot(true);
  // Async webserver
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    request->send(200, "text/html", "<form>Text to Broadcast<br><input type='text' name='BROADCAST'><br><br><input type='submit' value='Submit'></form>");
    if (request->hasArg("BROADCAST"))
      {
      String msg = request->arg("BROADCAST");
      mesh.sendBroadcast(msg);
      } });
  server.on("/map", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send_P(200, "text/html", "<html><head><script type='text/javascript' src='https://cdnjs.cloudflare.com/ajax/libs/vis/4.21.0/vis.js'></script><link href='https://cdnjs.cloudflare.com/ajax/libs/vis/4.21.0/vis-network.min.css' rel='stylesheet' type='text/css' /><style type='text/css'>#mynetwork {width: 1024px;height: 768px;border: 1px solid lightgray;}</style></head><body><h1>PainlessMesh Network Map</h1><div id='mynetwork'></div><a href=https://visjs.org>Made with vis.js<img src='http://www.davidefabbri.net/files/visjs_logo.png' width=40 height=40></a><script>var txt = '%SCAN%';</script><script type='text/javascript' src='http://www.davidefabbri.net/files/painlessmeshmap.js'></script></body></html>", scanprocessor); });
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json", mesh.subConnectionJson(false)); });
  /*server.on("/asnodetree", HTTP_GET, [](AsyncWebServerRequest *request)
    {
    request->send(200, "text/html", mesh.asNodeTree().c_str() );
    });*/
  server.begin();

  // if you want your node to accept OTA firmware, simply include this line
  // with whatever role you want your hardware to be. For instance, a
  // mesh network may have a thermometer, rain detector, and bridge. Each of
  // those may require different firmware, so different roles are preferrable.
  //
  // MAKE SURE YOUR UPLOADED OTA FIRMWARE INCLUDES OTA SUPPORT OR YOU WILL LOSE
  // THE ABILITY TO UPLOAD MORE FIRMWARE OVER OTA. YOU ALSO WANT TO MAKE SURE
  // THE ROLES ARE CORRECT
  mesh.initOTAReceive("bridge");

  sprintf(buff, "Id:%d", mesh.getNodeId());
}
void loop()
{
  // it will run the user scheduler as well
  mesh.update();

  if (myIP != getlocalIP())
  {
    myIP = getlocalIP();
    Serial.println("My IP is " + myIP.toString());
    initialized = 1;
  }
}