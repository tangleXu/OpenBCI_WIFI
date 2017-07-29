#include <time.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266SSDP.h>
#include <WiFiManager.h>
#include "SPISlave.h"
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>
#include <Hash.h>
// #include "WiFiClientPrint.h"
// #include "OpenBCI_Wifi_Definitions.h"
#include "OpenBCI_Wifi.h"



boolean isWaitingOnResetConfirm;
boolean ntpOffsetSet;
boolean underSelfTest;
boolean syncingNtp;
boolean tcpDelimiter;
boolean waitingOnNTP;

CLIENT_RESPONSE curClientResponse;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

String jsonStr;

uint8_t ntpTimeSyncAttempts;
uint8_t samplesLoaded;

unsigned long lastSendToClient;
unsigned long lastHeadMove;
unsigned long lastMQTTConnectAttempt;
unsigned long ntpLastTimeSeconds;

WiFiClient clientTCP;
WiFiClient espClient;
PubSubClient clientMQTT(espClient);

///////////////////////////////////////////
// Utility functions
///////////////////////////////////////////

boolean mqttConnect(String username, String password) {
  if (clientMQTT.connect(wifi.getName().c_str(), username, password)) {
#ifdef DEBUG
    Serial.println(JSON_CONNECTED);
#endif
    // Once connected, publish an announcement...
    clientMQTT.publish("openbci", "Will you Push The World?");
    return true;
  } else {
    // Wait 5 seconds before retrying
    lastMQTTConnectAttempt = millis();
    return false;
  }
}

uint8_t *giveMeASPIStreamPacket(uint8_t *data, uint8_t sampleNumber) {
  data[0] = STREAM_PACKET_BYTE_STOP;
  data[1] = sampleNumber;
  uint8_t index = 1;
  for (int i = 2; i < BYTES_PER_SPI_PACKET; i++) {
    data[i] = 0;
    if (i%3==2) {
      data[i] = index;
      index++;
    }
  }
  return data;
}

uint8_t *giveMeASPIStreamPacket(uint8_t *data) {
  return giveMeASPIStreamPacket(data, 0);
}


void printWifiStatus() {
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your WiFi shield's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);
}

///////////////////////////////////////////
// NTP BEGIN
///////////////////////////////////////////


/**
 * Use this to start the sntp time sync
 * @type {Number}
 */
void ntpStart() {
#ifdef DEBUG
  Serial.println("Setting time using SNTP");
#endif
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}


///////////////////////////////////////////////////
// MQTT
///////////////////////////////////////////////////

void callbackMQTT(char* topic, byte* payload, unsigned int length) {

#ifdef DEBUG
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
#endif
}

/**
 * Used when
 */
void configModeCallback (WiFiManager *myWiFiManager) {
#ifdef DEBUG
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
#endif
}


///////////////////////////////////////////////////
// HTTP Rest Helpers
///////////////////////////////////////////////////

/**
 * Returns true if there is no args on the POST request.
 */
boolean noBodyInParam() {
  return server.args() == 0;
}

void serverReturn(int code, String s) {
  digitalWrite(5, HIGH);
  server.send(code, "text/plain", s + "\r\n");
  digitalWrite(5, LOW);
}

void returnOK(String s) {
  serverReturn(200, s);
}

void returnOK(void) {
  returnOK("OK");
}

/**
 * Used to send a response to the client that the board is not attached.
 */
void returnNoSPIMaster() {
  if (lastTimeWasPolled < 1) {
    serverReturn(SPI_NO_MASTER, "Error: No OpenBCI board attached");
  } else {
    serverReturn(SPI_TIMEOUT_CLIENT_RESPONSE, "Error: Lost communication with OpenBCI board");
  }
}

/**
 * Used to send a response to the client that there is no body in the post request.
 */
void returnNoBodyInPost() {
  serverReturn(CLIENT_RESPONSE_NO_BODY_IN_POST, "Error: No body in POST request");
}

/**
 * Return if there is a missing param in the required command
 */
void returnMissingRequiredParam(const char *err) {
  serverReturn(CLIENT_RESPONSE_MISSING_REQUIRED_CMD, String(err));
}

void returnFail(int code, String msg) {
  digitalWrite(5, HIGH);
  server.send(code, "text/plain", msg + "\r\n");
  digitalWrite(5, LOW);
}

bool readRequest(WiFiClient& client) {
  bool currentLineIsBlank = true;
  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n' && currentLineIsBlank) {
        return true;
      } else if (c == '\n') {
        currentLineIsBlank = true;
      } else if (c != '\r') {
        currentLineIsBlank = false;
      }
    }
  }
  return false;
}

JsonObject& getArgFromArgs(int args) {
  DynamicJsonBuffer jsonBuffer(JSON_OBJECT_SIZE(args) + (40 * args));
  JsonObject& root = jsonBuffer.parseObject(server.arg(0));
  return root;
}

JsonObject& getArgFromArgs() {
  return getArgFromArgs(1);
}

/**
 * Used to set the latency of the system.
 */
void setLatency() {
  if (noBodyInParam()) return returnNoBodyInPost();

  JsonObject& root = getArgFromArgs();

  if (root.containsKey(JSON_LATENCY)) {
    wifi.setLatency(root[JSON_LATENCY]);
    returnOK();
  } else {
    returnMissingRequiredParam(JSON_LATENCY);
  }
}

/**
 * Used to set the latency of the system.
 */
void passthroughCommand() {
  if (noBodyInParam()) return returnNoBodyInPost();
  if (!wifi.spiHasMaster()) return returnNoSPIMaster();
  JsonObject& root = getArgFromArgs();

#ifdef DEBUG
  root.printTo(Serial);
#endif
  if (root.containsKey(JSON_COMMAND)) {
    String cmds = root[JSON_COMMAND];
    uint8_t retVal = wifi.passthroughCommands(cmds);
    if (retVal < PASSTHROUGH_PASS) {
      switch(retVal) {
        case PASSTHROUGH_FAIL_TOO_MANY_CHARS:
          return returnFail(501, "Error: Sent more than 31 chars");
        case PASSTHROUGH_FAIL_NO_CHARS:
          return returnFail(502, "Error: No characters found for key 'command'");
        case PASSTHROUGH_FAIL_QUEUE_FILLED:
          return returnFail(503, "Error: Queue is full, please wait 20ms and try again.");
        default:
          return returnFail(504, "Error: Unknown error");
      }
    }
    return;
  } else {
    return returnMissingRequiredParam(JSON_COMMAND);
  }
}

void setupSocketWithClient() {
  // Parse args
  if(noBodyInParam()) return returnNoBodyInPost(); // no body
  JsonObject& root = getArgFromArgs(5);
  if (!root.containsKey(JSON_TCP_IP)) return returnMissingRequiredParam(JSON_TCP_IP);
  String tempAddr = root[JSON_TCP_IP];
  IPAddress tempIPAddr;
  if (!tempIPAddr.fromString(tempAddr)) {
    return returnFail(505, "Error: unable to parse ip address. Please send as string in octets i.e. xxx.xxx.xxx.xxx");
  }
  if (!root.containsKey(JSON_TCP_PORT)) return returnMissingRequiredParam(JSON_TCP_PORT);
  int port = root[JSON_TCP_PORT];
  if (root.containsKey(JSON_TCP_OUTPUT)) {
    String outputModeStr = root[JSON_TCP_OUTPUT];
    if (outputModeStr.equals(getOutputMode(OUTPUT_MODE_RAW))) {
      wifi.setOutputMode(wifi.OUTPUT_MODE_RAW);
    } else if (outputModeStr.equals(getOutputMode(OUTPUT_MODE_JSON))) {
      wifi.setOutputMode(wifi.OUTPUT_MODE_JSON);
    } else {
      return returnFail(506, "Error: '" + String(JSON_TCP_OUTPUT) +"' must be either "+getOutputMode(OUTPUT_MODE_RAW)+" or "+getOutputMode(OUTPUT_MODE_JSON));
    }
#ifdef DEBUG
    Serial.print("Set output mode to "); Serial.println(getCurOutputMode());
#endif
  }

  if (root.containsKey(JSON_LATENCY)) {
    int latency = root[JSON_LATENCY];
    wifi.setLatency(latency);
#ifdef DEBUG
    Serial.print("Set latency to "); Serial.print(wifi.getLatency()); Serial.println(" uS");
#endif
  }
  boolean tcpDelimiter = wifi.tcpDelimiter;
  if (root.containsKey(JSON_TCP_DELIMITER)) {
    tcpDelimiter = root[JSON_TCP_DELIMITER];
#ifdef DEBUG
    Serial.print("Will use delimiter:"); Serial.println(wifi.tcpDelimiter ? "true" : "false");
#endif
  }

  wifi.setInfoTCP(tempAddr, port, tcpDelimiter);

#ifdef DEBUG
  Serial.print("Got ip: "); Serial.println(wifi.tcpAddress.toString());
  Serial.print("Got port: "); Serial.println(wifi.tcpPort);
  Serial.print("Current uri: "); Serial.println(server.uri());
  Serial.print("Starting socket to host: "); Serial.print(wifi.tcpAddress.toString()); Serial.print(" on port: "); Serial.println(wifi.tcpPort);
#endif

  wifi.setOutputProtocol(wifi.OUTPUT_PROTOCOL_TCP);

  if (clientTCP.connect(wifi.tcpAddress, wifi.tcpPort)) {
#ifdef DEBUG
    Serial.println("Connected to server");
#endif
    clientTCP.setNoDelay(1);
    return server.send(200, "text/json", wifi.tcpGetInfo());
  } else {
#ifdef DEBUG
    Serial.println("Failed to connect to server");
#endif
    return server.send(504, "text/json", wifi.tcpGetInfo());
  }
}

/**
 * Function called on route `/mqtt` with HTTP_POST with body
 * {"username":"user_name", "password": "you_password", "broker_address": "/your.broker.com"}
 */
void mqttSetup() {
  // Parse args
  if(noBodyInParam()) return returnNoBodyInPost(); // no body
  JsonObject& root = getArgFromArgs(5);
  //
  // size_t argBufferSize = JSON_OBJECT_SIZE(3) + 220;
  // DynamicJsonBuffer jsonBuffer(argBufferSize);
  // JsonObject& root = jsonBuffer.parseObject(server.arg(0));
  if (!root.containsKey(JSON_MQTT_USERNAME)) return returnMissingRequiredParam(JSON_MQTT_USERNAME);
  if (!root.containsKey(JSON_MQTT_PASSWORD)) return returnMissingRequiredParam(JSON_MQTT_PASSWORD);
  if (!root.containsKey(JSON_MQTT_BROKER_ADDR)) return returnMissingRequiredParam(JSON_MQTT_BROKER_ADDR);

  if (root.containsKey(JSON_LATENCY)) {
    latency = root[JSON_LATENCY];
#ifdef DEBUG
    Serial.print("Set latency to "); Serial.print(latency); Serial.println(" uS");
#endif
  }

  String mqttUsername = root[JSON_MQTT_USERNAME]; // "alongname.alonglastname@getcloudbrain.com"
  String mqttPassword = root[JSON_MQTT_PASSWORD]; // "that time when i had a big password"
  String mqttBrokerAddress = root[JSON_MQTT_BROKER_ADDR]; // "/the quick brown fox jumped over the lazy dog"

#ifdef DEBUG
  Serial.print("Got username: "); Serial.println(mqttUsername);
  Serial.print("Got password: "); Serial.println(mqttPassword);
  Serial.print("Got broker_address: "); Serial.println(mqttBrokerAddress);

  Serial.println("About to try and connect to cloudbrain MQTT server");
#endif

  wifi.setInfoMQTT(mqttBrokerAddress, mqttUsername, mqttPassword);
  clientMQTT.setServer(wifi.mqttBrokerAddress, 1883);
  if (mqttConnect(mqttUsername, mqttPassword)) {
    return server.send(200, "text/json", wifi.getInfoMqtt(true));
  } else {
    return server.send(505, "text/json", wifi.getInfoMqtt(false));
  }
}

void removeWifiAPInfo() {
  curClientResponse = CLIENT_RESPONSE_OUTPUT_STRING;
  wifi.outputString = "Forgetting wifi credentials and rebooting";
  wifi.clientWaitingForResponseFullfilled = true;

#ifdef DEBUG
  Serial.println(wifi.outputString);
  Serial.println(ESP.eraseConfig());
#else
  ESP.eraseConfig();
#endif
  delay(5000);
  ESP.reset();
}

void initializeVariables() {
  isWaitingOnResetConfirm = false;
  ntpOffsetSet = false;
  underSelfTest = false;
  syncingNtp = false;
  waitingOnNTP = false;

  lastHeadMove = 0;
  lastMQTTConnectAttempt = 0;
  lastSampleNumber = 0;
  lastSendToClient = 0;
  lastTimeWasPolled = 0;
  ntpLastTimeSeconds = 0;©
  ntpOffset = 0;
  ntpTimeSyncAttempts = 0;
  sampleNumber = 0;
  samplesLoaded = 0;

  jsonStr = "";
}

void setup() {
  initializeVariables();

#ifdef DEBUG
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("Serial started");
#endif

  wifi.begin();

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  WiFiManagerParameter custom_text("<p>Powered by Push The World</p>");
  wifiManager.addParameter(&custom_text);

  wifiManager.setAPCallback(configModeCallback);

  //and goes into a blocking loop awaiting configuration
#ifdef DEBUG
  Serial.println("Wifi manager started...");
#endif
  wifiManager.autoConnect(getName().c_str());

#ifdef DEBUG
  Serial.printf("Starting ntp...\n");
#endif
  wifi.ntpStart();

#ifdef DEBUG
  Serial.printf("Starting SSDP...\n");
#endif
  SSDP.setSchemaURL("description.xml");
  SSDP.setHTTPPort(80);
  SSDP.setName("PTW - OpenBCI Wifi Shield");
  SSDP.setSerialNumber(wifi.getName());
  SSDP.setURL("index.html");
  SSDP.setModelName(wifi.getModelNumber());
  SSDP.setModelNumber("929000226503");
  SSDP.setModelURL("http://www.openbci.com");
  SSDP.setManufacturer("Push The World LLC");
  SSDP.setManufacturerURL("http://www.pushtheworldllc.com");
  SSDP.begin();

  // pinMode(0, INPUT);
  // data has been received from the master. Beware that len is always 32
  // and the buffer is autofilled with zeroes if data is less than 32 bytes long
  // It's up to the user to implement protocol for handling data length
  SPISlave.onData([](uint8_t * data, size_t len) {
    wifi.spiProcessPacket(data);
  });

  SPISlave.onDataSent([]() {
    wifi.spiOnDataSent();
    SPISlave.setData(wifi.passthroughBuffer, BYTES_PER_SPI_PACKET);
  });

  // The master has read the status register
  SPISlave.onStatusSent([]() {
#ifdef DEBUG
    Serial.println("Status Sent");
#endif
    SPISlave.setStatus(209);
  });

  // Setup SPI Slave registers and pins
  SPISlave.begin();

  // Set the status register (if the master reads it, it will read this value)
  SPISlave.setStatus(209);
  SPISlave.setData(wifi.passthroughBuffer, BYTES_PER_SPI_PACKET);

#ifdef DEBUG
  Serial.println("SPI Slave ready");
  printWifiStatus();
  Serial.printf("Starting HTTP...\n");
#endif
  server.on("/", HTTP_GET, [](){
    digitalWrite(5, HIGH);
    server.send(200, "text/plain", "Push The World - Please visit https://app.swaggerhub.com/apis/pushtheworld/openbci-wifi-server/1.3.0 for the latest HTTP requests");
    digitalWrite(5, LOW);
  });
  server.on("/index.html", HTTP_GET, [](){
    digitalWrite(5, HIGH);
    server.send(200, "text/plain", "Push The World - OpenBCI - Wifi bridge - is up and running woo");
    digitalWrite(5, LOW);
  });
  server.on("/description.xml", HTTP_GET, [](){
#ifdef DEBUG
    Serial.println("SSDP HIT");
#endif
    digitalWrite(5, HIGH);
    SSDP.schema(server.client());
    digitalWrite(5, LOW);
  });
  server.on("/yt", HTTP_GET, [](){
    digitalWrite(5, HIGH);
    server.send(200, "text/plain", "Keep going! Push The World!");
    digitalWrite(5, LOW);
  });

  server.on("/test/start", HTTP_GET, [](){
    underSelfTest = true;
#ifdef DEBUG
    Serial.println("Under self test start");
#endif
    returnOK();
  });
  server.on("/test/stop", HTTP_GET, [](){
    underSelfTest = false;
#ifdef DEBUG
    Serial.println("Under self test start");
#endif
    returnOK();
  });

  server.on("/output/json", HTTP_GET, [](){
    wifi.setOutputMode(OUTPUT_MODE_JSON);
    returnOK();
  });
  server.on("/output/raw", HTTP_GET, [](){
    wifi.setOutputMode(OUTPUT_MODE_RAW);
    returnOK();
  });

  server.on("/mqtt", HTTP_GET, []() {
    server.send(200, "text/json", wifi.mqttGetInfo(clientMQTT.connected()));
  });
  server.on("/mqtt", HTTP_POST, mqttSetup);

  server.on("/tcp", HTTP_GET, []() {
    server.send(200, "text/json", wifi.tcpGetInfo(clientTCP.connected()));
  });
  server.on("/tcp", HTTP_POST, setupSocketWithClient);
  server.on("/tcp", HTTP_DELETE, []() {
    clientTCP.stop();
    server.send(200, "text/json", wifi.tcpGetInfo(clientTCP.connected()));
  });

  // These could be helpful...
  server.on("/stream/start", HTTP_GET, []() {
    wifi.passthroughCommands("b");
    returnOK();
  });
  server.on("/stream/stop", HTTP_GET, []() {
    wifi.passthroughCommands("b");
    returnOK();
  });

  server.on("/version", HTTP_GET, [](){
    digitalWrite(5, HIGH);
    server.send(200, "text/plain", wifi.getVersion());
    digitalWrite(5, LOW);
  });

  server.on("/command", HTTP_POST, passThroughCommand);

  server.on("/latency", HTTP_GET, [](){
    returnOK(String(wifi.getLatency()).c_str());
  });
  server.on("/latency", HTTP_POST, setLatency);

  if (!MDNS.begin(wifi.getName().c_str())) {
#ifdef DEBUG
    Serial.println("Error setting up MDNS responder!");
#endif
  } else {
#ifdef DEBUG
    Serial.print("Your ESP is called "); Serial.println(wifi.getName());
#endif
  }

  server.onNotFound([](){
    server.send(404, "text/plain", "Route Not Found");
  });
  // server.onNotFound(handleNotFound);
  //
  //get heap status, analog input value and all GPIO statuses in one json call
  server.on("/all", HTTP_GET, [](){
    String output = wifi.getInfoAll();
    server.setContentLength(output.length());
    server.send(200, "text/json", output);
#ifdef DEBUG
    Serial.println(output);
#endif
  });

  server.on("/board", HTTP_GET, [](){
    String output = wifi.getInfoBoard();
    server.setContentLength(output.length());
    server.send(200, "text/json", output);
#ifdef DEBUG
    Serial.println(output);
#endif
  });

  server.on("/wifi", HTTP_DELETE, []() {
    pinMode(0, INPUT);
    isWaitingOnResetConfirm = true;
    clientWaitingForResponseFullfilled = false;
#ifdef DEBUG
    Serial.println("waiting for button press");
#endif
  });

  httpUpdater.setup(&server);

  server.begin();
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("ws", "tcp", 81);

#ifdef DEBUG
    Serial.printf("Ready!\n");
#endif

  // Test to see if ntp is good
  if (wifi.ntpActive()) {
    syncingNtp = true;
  } else {
#ifdef DEBUG
    Serial.println("Unable to get ntp sync");
#endif
    waitingOnNTP = true;
    ntpLastTimeSeconds = millis();
  }

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  clientMQTT.setCallback(callbackMQTT);

#ifdef DEBUG
  Serial.println("Spi has master: " + String(wifi.spiHasMaster() ? "true" : "false"));
#endif

}

/////////////////////////////////
/////////////////////////////////
// LOOP LOOP LOOP LOOP LOOP /////
/////////////////////////////////
/////////////////////////////////
void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  if (wifi.curOutputProtocol == OUTPUT_PROTOCOL_MQTT) {
    if (clientMQTT.connected()) {
      clientMQTT.loop();
    } else if (millis() > 5000 + lastMQTTConnectAttempt) {
      mqttConnect(wifi.mqttUsername, wifi.mqttPassword);
    }
  }

  if (isWaitingOnResetConfirm) {
    if (digitalRead(0)==0) {
      isWaitingOnResetConfirm = false;
      removeWifiAPInfo();
#ifdef DEBUG
    } else {
      Serial.print(".");
      delay(1);
#endif
    }
  }

  if (syncingNtp) {
    unsigned long long curTime = time(nullptr);
    if (ntpLastTimeSeconds == 0) {
      ntpLastTimeSeconds = curTime;
    } else if (ntpLastTimeSeconds < curTime) {
      wifi.setNTPOffset(micros() % MICROS_IN_SECONDS);
      syncingNtp = false;
      ntpOffsetSet = true;

#ifdef DEBUG
      Serial.print("\nTime set: "); Serial.println(wifi.getNTPOffset());
#endif
    }
  }

  if (waitingOnNTP && (millis() > 3000 + ntpLastTimeSeconds)) {
    // Test to see if ntp is good
    if (ntpActive()) {
      waitingOnNTP = false;
      syncingNtp = true;
      ntpLastTimeSeconds = 0;
    }
    ntpTimeSyncAttempts++;
    if (ntpTimeSyncAttempts > 10) {
#ifdef DEBUG
      Serial.println("Unable to get ntp sync");
#endif
      waitingOnNTP = false;
    } else {
      ntpLastTimeSeconds = millis();
    }
  }

  if (wifi.clientWaitingForResponseFullfilled) {
    wifi.clientWaitingForResponseFullfilled = false;
    switch (wifi.curClientResponse) {
      case CLIENT_RESPONSE_OUTPUT_STRING:
        returnOK(wifi.outputString);
        wifi.outputString = "";
        break;
      case CLIENT_RESPONSE_NONE:
      default:
        returnOK();
        break;
    }
  }

  if (underSelfTest) {
    int sampleIntervaluS = 1000; // micro seconds
    boolean daisy = false;
    if (micros() > (lastHeadMove + sampleIntervaluS)) {
      uint8_t data[BYTES_PER_SPI_PACKET];
      giveMeASPIStreamPacket(data, wifi.lastSampleNumber);
      // wifi.head += daisy ? 2 : 1;
      // if (head >= NUM_PACKETS_IN_RING_BUFFER) {
      //   head -= NUM_PACKETS_IN_RING_BUFFER;
      // }
      lastHeadMove = micros();
    }
  }

  if (wifi.clientWaitingForResponse && (millis() > (wifi.timePassthroughBufferLoaded + 1000))) {
    wifi.clientWaitingForResponse = false;
    returnFail(502, "Error: timeout getting command response, be sure board is fully connected");
#ifdef DEBUG
    Serial.println("Failed to get response in 1000ms");
#endif
  }

  if((clientTCP.connected() || clientMQTT.connected() || curOutputProtocol == OUTPUT_PROTOCOL_SERIAL) && (micros() > (lastSendToClient + latency)) && head != tail) {
    // Serial.print("h: "); Serial.print(head); Serial.print(" t: "); Serial.print(tail); Serial.print(" cTCP: "); Serial.print(clientTCP.connected()); Serial.print(" cMQTT: "); Serial.println(clientMQTT.connected());

    digitalWrite(5, HIGH);

    int packetsToSend = head - tail;

    if (wifi.curOutputMode == wifi.OUTPUT_MODE_RAW) {
      for(int i = 0; i < 2; i++) {
        if (wifi.rawBufferHasData(wifi.rawBuffer + i)) {
          if (wifi.curOutputProtocol == wifi.OUTPUT_PROTOCOL_TCP) {
            clientTCP.write((wifi.rawBuffer + i)->data, (wifi.rawBuffer + i)->positionWrite);
            if (wifi.tcpDelimiter) {
              clientTCP.write("\r\n");
            }
          } else if (wifi.curOutputProtocol == wifi.OUTPUT_PROTOCOL_MQTT) {
            clientMQTT.publish("openbci",(const char*)(wifi.rawBuffer + i)->data);
          } else {
            Serial.println((const char*)(wifi.rawBuffer + i)->data);
          }
          wifi.rawBufferReset(wifi.rawBuffer + i);
        }
      }
    } else { // output mode is JSON
      if (packetsToSend < 0) {
        packetsToSend = NUM_PACKETS_IN_RING_BUFFER_JSON + packetsToSend; // for wrap around
      }
      if (packetsToSend > wifi.getJSONMaxPackets()) {
        packetsToSend = wifi.getJSONMaxPackets();
      }

      DynamicJsonBuffer jsonSampleBuffer(wifi.getJSONBufferSize());
      JsonObject& root = jsonSampleBuffer.createObject();

      wifi.getJSONFromSamples(root, wifi.numChannels, packetsToSend);


      if (wifi.curOutputProtocol == wifi.OUTPUT_PROTOCOL_TCP) {
        // root.printTo(Serial);
        WiFiClientPrint<> tcpBufferedPrinter(clientTCP);
        root.printTo(tcpBufferedPrinter);
        tcpBufferedPrinter.flush();
        if (wifi.tcpDelimiter) {
          clientTCP.write("\r\n");
        }
      } else if (wifi.curOutputProtocol == wifi.OUTPUT_PROTOCOL_MQTT) {
        jsonStr = "";
        root.printTo(jsonStr);
        clientMQTT.publish("openbci", jsonStr.c_str());
        jsonStr = "";
      } else {
        root.printTo(jsonStr);
        Serial.println(jsonStr);
        jsonStr = "";
        // root.printTo(Serial);
        Serial.println();
      }
    }

    digitalWrite(5, LOW);

    // clientTCP.write(outputBuf, BYTES_PER_SPI_PACKET * packetsToSend);
    lastSendToClient = micros();
  }
}
