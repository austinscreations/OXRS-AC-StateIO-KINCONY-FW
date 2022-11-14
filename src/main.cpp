/**
  KinCony esp32 128 channel relay module kc868 a128
  firmware for the Open eXtensible Rack System

  Documentation:  
    TO DO
  
  GitHub repository:
    https://github.com/austinscreations/OXRS-AC-kinconyStateIO-ESP32-FW

  Copyright 2022 Austins Creations
*/

/*----------------------- Connection Type -----------------------------*/
//#define ETHMODE
//#define WIFIMODE

/*------------------------- I2C pins ----------------------------------*/
//#define I2C_SDA   0
//#define I2C_SCL   1

//rack32   = 21  22
//LilyGO   = 33  32
//room8266 =  4   5
//D1 mini  =  4   0

/*--------------------------- Macros ----------------------------------*/
#define STRINGIFY(s) STRINGIFY1(s)
#define STRINGIFY1(s) #s

/*--------------------------- Libraries -------------------------------*/
#include <Arduino.h>
#include <Wire.h>                   // For I2C
#include <Adafruit_PCF8575.h>       // for I/O handling
#include <OXRS_Input.h>             // For input handling
#include <OXRS_Output.h>              // For output handling
#include <PubSubClient.h>           // For MQTT
#include <OXRS_MQTT.h>              // For MQTT
#include <OXRS_API.h>               // For REST API
#include <WiFiManager.h>            // captive wifi AP config
#include <MqttLogger.h>             // for mqtt and serial logging

#include <WiFi.h>                   // For networking
#if defined(ETHMODE)
#include <ETH.h>                    // For networking
#include <SPI.h>                    // For ethernet
#endif

/*--------------------------- Constants ----------------------------------*/
// TWO i2c buses
TwoWire Wire  = TwoWire(0);
TwoWire Wire1 = TwoWire(1);

// Serial
#define SERIAL_BAUD_RATE            115200

// REST API
#define REST_API_PORT               80

// Each CPF8575 has 16 I/O pins
#define PCF_PIN_COUNT               16

// Set false for breakout boards with external pull-ups
#define PCF_INTERNAL_PULLUPS        true

// Internal constant used when input type parsing fails
#define INVALID_INPUT_TYPE          99

// Internal constants used when output type parsing fails
#define       INVALID_OUTPUT_TYPE   99

// Can have up to 8x PCF8575 on a single I2C bus
const byte    PCF_I2C_ADDRESS[]     = { 0x24, 0x25, 0x21, 0x22, 0x26, 0x27, 0x20, 0x23 };
const uint8_t PCF_COUNT             = sizeof(PCF_I2C_ADDRESS);

// Ethernet
#if defined(ETHMODE)
#define DHCP_TIMEOUT_MS             15000
#define DHCP_RESPONSE_TIMEOUT_MS    4000

#define ETH_CLOCK_MODE              ETH_CLOCK_GPIO17_OUT   // Version with not PSRAM
#define ETH_PHY_TYPE                ETH_PHY_LAN8720        // Type of the Ethernet PHY (LAN8720 or TLK110)  
#define ETH_PHY_POWER               -1                     // Pin# of the enable signal for the external crystal oscillator (-1 to disable for internal APLL source)
#define ETH_PHY_MDC                 23                     // Pin# of the I²C clock signal for the Ethernet PHY
#define ETH_PHY_MDIO                18                     // Pin# of the I²C IO signal for the Ethernet PHY
#define ETH_PHY_ADDR                0                      // I²C-address of Ethernet PHY (0 or 1 for LAN8720, 31 for TLK110)
#define ETH_RST_PIN                 5

#endif

/*-------------------------- Internal datatypes --------------------------*/


/*--------------------------- Global Variables ---------------------------*/
// OUTPUTS - Each bit corresponds to an PCF found on the I2C bus
uint8_t g_pcfs_found_do = 0;

// INPUTS - Each bit corresponds to an PCF found on the I2C bus
uint8_t g_pcfs_found_di = 0;

// How many pins on each MCP are we controlling (defaults to all 16)
// Set via "outputsPerMcp" integer config option - should be set via
// the REST API so it is persisted to SPIFFS and loaded early enough
// in the boot sequence to configure the LCD and adoption payloads
uint8_t g_pcf_output_pins = PCF_PIN_COUNT;

/*--------------------------- Instantiate Global Objects -----------------*/
// I/O buffers
Adafruit_PCF8575 pcf8575_DO[PCF_COUNT]; // Output - wire bus 0
Adafruit_PCF8575 pcf8575_DI[PCF_COUNT]; // input - wire bus 1

// Input handlers
OXRS_Input oxrsInput[PCF_COUNT];

// Output handlers
OXRS_Output oxrsOutput[PCF_COUNT];

#if defined(ETHMODE)
WiFiClient client;
WiFiServer server(REST_API_PORT);
#endif

#if defined(WIFIMODE)
WiFiClient client;
WiFiServer server(REST_API_PORT);
#endif

// MQTT
PubSubClient mqttClient(client);
OXRS_MQTT mqtt(mqttClient);

// REST API
OXRS_API api(mqtt);

// Logging
MqttLogger logger(mqttClient, "log", MqttLoggerMode::MqttAndSerial);

/*--------------------------- JSON builders -----------------*/
void getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json.createNestedObject("firmware");

  firmware["name"] = FW_NAME;
  firmware["shortName"] = FW_SHORT_NAME;
  firmware["maker"] = FW_MAKER;
  firmware["version"] = STRINGIFY(FW_VERSION);

#if defined(FW_GITHUB_URL)
  firmware["githubUrl"] = FW_GITHUB_URL;
#endif
}

void getSystemJson(JsonVariant json)
{
  JsonObject system = json.createNestedObject("system");

  system["flashChipSizeBytes"] = ESP.getFlashChipSize();
  system["heapFreeBytes"] = ESP.getFreeHeap();

  system["heapUsedBytes"] = ESP.getHeapSize();
  system["heapMaxAllocBytes"] = ESP.getMaxAllocHeap();

  system["sketchSpaceUsedBytes"] = ESP.getSketchSize();
  system["sketchSpaceTotalBytes"] = ESP.getFreeSketchSpace();

  system["fileSystemUsedBytes"] = SPIFFS.usedBytes();
  system["fileSystemTotalBytes"] = SPIFFS.totalBytes();

}

void getNetworkJson(JsonVariant json)
{
  JsonObject network = json.createNestedObject("network");
  
  byte mac[6]; 

  #if defined(ETHMODE)
  network["mode"] = "ethernet";
  network["ip"] = ETH.localIP();
  network["mac"] = ETH.macAddress();

  #elif defined(WIFIMODE)
  network["mode"] = "wifi";
  WiFi.macAddress(mac);
  network["ip"] = WiFi.localIP();
  #endif
  
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  network["mac"] = mac_display;
}

void getConfigSchemaJson(JsonVariant json)
{
  JsonObject configSchema = json.createNestedObject("configSchema");
  
  // Config schema metadata
  configSchema["$schema"] = JSON_SCHEMA_VERSION;
  configSchema["title"] = FW_SHORT_NAME;
  configSchema["type"] = "object";

  JsonObject properties = configSchema.createNestedObject("properties");

}

void getCommandSchemaJson(JsonVariant json)
{
  JsonObject commandSchema = json.createNestedObject("commandSchema");
  
  // Command schema metadata
  commandSchema["$schema"] = JSON_SCHEMA_VERSION;
  commandSchema["title"] = FW_SHORT_NAME;
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema.createNestedObject("properties");
  
}

void apiAdopt(JsonVariant json)
{
  // Build device adoption info
  getFirmwareJson(json);
  getSystemJson(json);
  getNetworkJson(json);
  getConfigSchemaJson(json);
  getCommandSchemaJson(json);
}

/*--------------------------- Initialisation -------------------------------*/
void initialiseRestApi(void)
{
  // NOTE: this must be called *after* initialising MQTT since that sets
  //       the default client id, which has lower precendence than MQTT
  //       settings stored in file and loaded by the API

  // Set up the REST API
  api.begin();

  // Register our callbacks
  api.onAdopt(apiAdopt);

  server.begin();
}

/*--------------------------- MQTT/API -----------------*/
void mqttConnected() 
{
  // MqttLogger doesn't copy the logging topic to an internal
  // buffer so we have to use a static array here
  static char logTopic[64];
  logger.setTopic(mqtt.getLogTopic(logTopic));

  // Publish device adoption info
  DynamicJsonDocument json(JSON_ADOPT_MAX_SIZE);
  mqtt.publishAdopt(api.getAdopt(json.as<JsonVariant>()));

  // Log the fact we are now connected
  logger.println("[stio] mqtt connected");
}

void mqttDisconnected(int state) 
{
  // Log the disconnect reason
  // See https://github.com/knolleary/pubsubclient/blob/2d228f2f862a95846c65a8518c79f48dfc8f188c/src/PubSubClient.h#L44
  switch (state)
  {
    case MQTT_CONNECTION_TIMEOUT:
      logger.println(F("[stio] mqtt connection timeout"));
      break;
    case MQTT_CONNECTION_LOST:
      logger.println(F("[stio] mqtt connection lost"));
      break;
    case MQTT_CONNECT_FAILED:
      logger.println(F("[stio] mqtt connect failed"));
      break;
    case MQTT_DISCONNECTED:
      logger.println(F("[stio] mqtt disconnected"));
      break;
    case MQTT_CONNECT_BAD_PROTOCOL:
      logger.println(F("[stio] mqtt bad protocol"));
      break;
    case MQTT_CONNECT_BAD_CLIENT_ID:
      logger.println(F("[stio] mqtt bad client id"));
      break;
    case MQTT_CONNECT_UNAVAILABLE:
      logger.println(F("[stio] mqtt unavailable"));
      break;
    case MQTT_CONNECT_BAD_CREDENTIALS:
      logger.println(F("[stio] mqtt bad credentials"));
      break;      
    case MQTT_CONNECT_UNAUTHORIZED:
      logger.println(F("[stio] mqtt unauthorised"));
      break;      
  }
}

void jsonCommand(JsonVariant json){}

void jsonConfig(JsonVariant json){}

void mqttCallback(char * topic, uint8_t * payload, unsigned int length) 
{
  // Pass this message down to our MQTT handler
  mqtt.receive(topic, payload, length);
}

void initialiseMqtt(byte * mac)
{
  // Set the default client id to the last 3 bytes of the MAC address
  char clientId[32];
  sprintf_P(clientId, PSTR("%02x%02x%02x"), mac[3], mac[4], mac[5]);  
  mqtt.setClientId(clientId);
  
  // Register our callbacks
  mqtt.onConnected(mqttConnected);
  mqtt.onDisconnected(mqttDisconnected);
  mqtt.onConfig(jsonConfig);
  mqtt.onCommand(jsonCommand);  

  // Start listening for MQTT messages
  mqttClient.setCallback(mqttCallback);  
}

/*--------------------------- Network -------------------------------*/
#if defined(WIFIMODE)
void initialiseWifi()
{
  // Ensure we are in the correct WiFi mode
  WiFi.mode(WIFI_STA);

  // Get WiFi base MAC address
  byte mac[6];
  WiFi.macAddress(mac);

  // Display the MAC address on serial
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  logger.print(F("[stio] mac address: "));
  logger.println(mac_display);

  // Connect using saved creds, or start captive portal if none found
  // Blocks until connected or the portal is closed
  WiFiManager wm;
  if (!wm.autoConnect("OXRS_WiFi", "superhouse"))
  {
    // If we are unable to connect then restart
    ESP.restart();
  }

  // Display IP address on serial
  logger.print(F("[stio] ip address: "));
  logger.println(WiFi.localIP());

  // Set up MQTT (don't attempt to connect yet)
  initialiseMqtt(mac);

  // Set up the REST API once we have an IP address
  initialiseRestApi();
}
#endif

#if defined(ETHMODE)
void ethernetEvent(WiFiEvent_t event)
{
  // Log the event to serial for debugging
  switch (event)
  {
    case ARDUINO_EVENT_ETH_START:
      // Get the ethernet MAC address
      byte mac[6];
      ETH.macAddress(mac);

      // Display the MAC address on serial
      char mac_display[18];
      sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      logger.print(F("[stio] mac address: "));
      logger.println(mac_display);

      // Set up MQTT (don't attempt to connect yet)
      initialiseMqtt(mac);
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      // Get the IP address assigned by DHCP
      IPAddress ip = ETH.localIP();

      logger.print(F("[stio] ip address: "));
      logger.println(ip);
      
      // Set up the REST API once we have an IP address
      initialiseRestApi();
      break;
  }
}

void initialiseEthernet()
{
  // We continue initialisation inside this event handler
  WiFi.onEvent(ethernetEvent);

  // Reset the Ethernet PHY
  pinMode(ETH_RST_PIN, OUTPUT);
  digitalWrite(ETH_RST_PIN, 0);
  delay(200);
  digitalWrite(ETH_RST_PIN, 1);
  delay(200);
  digitalWrite(ETH_RST_PIN, 0);
  delay(200);
  digitalWrite(ETH_RST_PIN, 1);

  // Start the Ethernet PHY and wait for events
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLOCK_MODE);

}
#endif

void initialiseSerial()
{
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);
  
  logger.println(F("[stio] starting up..."));

  DynamicJsonDocument json(128);
  getFirmwareJson(json.as<JsonVariant>());

  logger.print(F("[stio] "));
  serializeJson(json, logger);
  logger.println();
}

/*--------------------------- Event Handler -------------------------------*/
void inputEvent(uint8_t id, uint8_t input, uint8_t type, uint8_t state)
{
  // Determine the index for this input event (1-based)
  uint8_t pcf = id;
  uint8_t index = (PCF_PIN_COUNT * pcf) + input + 1;

  // Publish the event
  // publishEvent(index, type, state);
}

void outputEvent(uint8_t id, uint8_t output, uint8_t type, uint8_t state)
{
  // Determine the index (1-based)
  uint8_t pcf = id;
  uint8_t pin = output;
  uint8_t raw_index = (g_pcf_output_pins * pcf) + pin;
  uint8_t index = raw_index + 1;
  
  // Update the MCP pin - i.e. turn the relay on/off (LOW/HIGH)
  pcf8575_DO[pcf].digitalWrite(pin, state);

  // Publish the event
  // publishEvent(index, type, state);
}

/*--------------------------- I2C -------------------------------*/
void scanI2CBus()
{
  logger.println(F("[stio] scanning for output buffers..."));

  for (uint8_t pcf1 = 0; pcf1 < PCF_COUNT; pcf1++)
  {
    logger.print(F(" - 0x"));
    logger.print(PCF_I2C_ADDRESS[pcf1], HEX);
    logger.print(F("..."));

    // Check if there is anything responding on this address
    Wire.beginTransmission(PCF_I2C_ADDRESS[pcf1]);
    if (Wire.endTransmission() == 0)
    {
      bitWrite(g_pcfs_found_do, pcf1, 1);

      // If an MCP23017 was found then initialise and configure the outputs
      pcf8575_DO[pcf1].begin(PCF_I2C_ADDRESS[pcf1],&Wire);
      for (uint8_t pin = 0; pin < PCF_PIN_COUNT; pin++)
      {
        pcf8575_DO[pcf1].pinMode(pin, OUTPUT);
        pcf8575_DO[pcf1].digitalWrite(pin, RELAY_OFF);
      }

      // Initialise output handlers
      oxrsOutput[pcf1].begin(outputEvent, RELAY);
      
      logger.println(F("PCF8575"));
    }
    else
    {
      logger.println(F("empty"));
    }
  }

  logger.println(F("[stio] scanning for input buffers..."));

  for (uint8_t pcf2 = 0; pcf2 < PCF_COUNT; pcf2++)
  {
    logger.print(F(" - 0x"));
    logger.print(PCF_I2C_ADDRESS[pcf2], HEX);
    logger.print(F("..."));

    // Check if there is anything responding on this address
    Wire1.beginTransmission(PCF_I2C_ADDRESS[pcf2]);
    if (Wire1.endTransmission() == 0)
    {
      bitWrite(g_pcfs_found_di, pcf2, 1);
      
      // If an MCP23017 was found then initialise and configure the inputs
      pcf8575_DI[pcf2].begin(PCF_I2C_ADDRESS[pcf2],&Wire1);
      for (uint8_t pin = 0; pin < PCF_PIN_COUNT; pin++)
      {
        pcf8575_DI[pcf2].pinMode(pin, PCF_INTERNAL_PULLUPS ? INPUT_PULLUP : INPUT);
      }

      // Initialise input handlers (default to SWITCH)
      oxrsInput[pcf2].begin(inputEvent, SWITCH);

      logger.print(F("PCF8575"));
      if (PCF_INTERNAL_PULLUPS) { logger.print(F(" (internal pullups)")); }
      logger.println();
    }
    else
    {
      logger.println(F("empty"));
    }
  }
}

/*--------------------------- Program -------------------------------*/
void setup()
{
  // Set up serial
  initialiseSerial();  

  // Start the I2C bus
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire1.begin(I2C_SDA2, I2C_SCL2);

  // Scan the I2C bus and set up I/O buffers
  scanI2CBus();

  // Set up network/MQTT/REST API
  #if defined(WIFIMODE)
  initialiseWifi();
  #elif defined(ETHMODE)
  initialiseEthernet();
  #endif
}

void loop()
{
  // Check our MQTT broker connection is still ok
  mqtt.loop();
  
  // Handle any API requests
  WiFiClient client = server.available();
  api.loop(&client);

  // OUTPUTS - Iterate through each of the MCP23017s
  for (uint8_t pcf1 = 0; pcf1 < PCF_COUNT; pcf1++)
  {
    if (bitRead(g_pcfs_found_do, pcf1) == 0) 
      continue;
    
    // Check for any output events
    oxrsOutput[pcf1].process();
  }

  // INPUTS - Iterate through each of the MCP23017s
  for (uint8_t pcf2 = 0; pcf2 < PCF_COUNT; pcf2++)
  {
    if (bitRead(g_pcfs_found_di, pcf2) == 0)
      continue;

    // Read the values for all 16 pins on this MCP
    uint16_t io_value = pcf8575_DI[pcf2].digitalReadWord();

    // Check for any input events
    oxrsInput[pcf2].process(pcf2, io_value);
  }
}