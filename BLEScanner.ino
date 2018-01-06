#include <Arduino.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <rom/rtc.h>
#include <deque>
#include <list>
#include <algorithm>

const char* ssid     = "ENTER_YOUR_WIFI_SSID_HERE";
const char* password = "ENTER_YOUR_WIFI_PASSWORD_HERE";
const char* hostname = "ENTER_THE_HOSTNAME_HERE";
uint32_t tagtimeout = 180; // Remove BLETag from list if not seen for 180 seconds

bool useStaticIP = false;
IPAddress local_IP(192, 168, 178, 78);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 178, 1); //optional
IPAddress secondaryDNS(8, 8, 8, 8); //optional
WiFiServer server(3333, 16);  // Port, Maxclients
int scanTime = 10; //In seconds


static BLEClient* pClient;
static BLEAddress* pAddress;
static BLERemoteService* pRemoteService;
static BLERemoteCharacteristic* pRemoteCharacteristic;

bool updating = false;
bool doScan = false;

struct BLETag {
  std::string mac;
  uint32_t lastseen;
  std::string tname;
  std::deque<int> rssi;
};

std::list<BLETag> bletags;
std::list<BLETag>::iterator itags;


class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks
{
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      updating = true;
      for (itags = bletags.begin(); itags != bletags.end(); ++itags) {
        if((*itags).mac == advertisedDevice.getAddress().toString()) {
          (*itags).lastseen = xTaskGetTickCount();
          if (advertisedDevice.getName().length() > 0) {
            (*itags).tname = advertisedDevice.getName();
          }
          (*itags).rssi.push_back(advertisedDevice.getRSSI());
          if((*itags).rssi.size() > 5) (*itags).rssi.pop_front();
          updating = false;
          return;
        }
      }
      std::string nmac = advertisedDevice.getAddress().toString();
      std::transform(nmac.begin(), nmac.end(), nmac.begin(), ::tolower);
      bletags.push_back(BLETag{nmac,
                               xTaskGetTickCount(),
                               advertisedDevice.getName(),
                               std::deque<int>{advertisedDevice.getRSSI()}});
      updating = false;
    }
};

void cleanupTags()
{
  updating = true;
  for (itags = bletags.begin(); itags != bletags.end();) {
    if(xTaskGetTickCount() < (*itags).lastseen) // uptime counter overflow
      (*itags).lastseen = xTaskGetTickCount();
    if((xTaskGetTickCount() - (*itags).lastseen) / 1000 > tagtimeout) {
      bletags.erase(itags++);
    } else {
      ++itags;
    }
  }
  updating = false;
}


void bleTask(void * pvParameters)
{
  Serial.println(F("Starting BLE init"));
  BLEDevice::init("");
  BLEScan* pBLEScan;
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  for (;;) {
    while(! doScan)
      delay(500);
    BLEScanResults foundDevices = pBLEScan->start(scanTime);
    Serial.print(F("Free heap: "));
    Serial.println(esp_get_free_heap_size());
    dumpTags();
    cleanupTags();
    delay(1000);
  }
}


void WiFiEvent(WiFiEvent_t event)
{
  switch (event) {
    case SYSTEM_EVENT_STA_START:
      WiFi.setHostname(hostname);
      Serial.println(F("SYSTEM_EVENT_STA_START"));
      break;
    case SYSTEM_EVENT_STA_CONNECTED:
      Serial.println(F("SYSTEM_EVENT_STA_CONNECTED"));
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println(F("SYSTEM_EVENT_STA_DISCONNECTED"));
      WiFi.reconnect();
      doScan = false;
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.println(F("SYSTEM_EVENT_STA_GOT_IP"));
      doScan = true;
      break;
    default:
      Serial.print(F("STA other event: "));
      Serial.println(event);
      break;
  }
}

void dumpTags()
{
  Serial.println("Tags found (MAC, Timestamp, Name, RSSIs):");
  while(updating) delay(10);
  for (itags = bletags.begin(); itags != bletags.end(); ++itags) {
    Serial.print("  ");
    Serial.print((*itags).mac.c_str());
    Serial.print("; ");
    Serial.print((*itags).lastseen);
    Serial.print("; ");
    Serial.print((*itags).tname.c_str());
    Serial.print("; >");
    std::deque<int>::iterator irssi;
    for(irssi = (*itags).rssi.begin(); irssi != (*itags).rssi.end(); ++irssi) {
      Serial.print((*irssi));
      Serial.print(",");
    }
    Serial.println("<");
  }
  Serial.println("");
}

void printTags(WiFiClient* client, std::string mac, short timeout)
{
  String msg = F("absence;rssi=unreachable;daemon=BLEScanner V0.1");
  while(updating) delay(10);
  for (itags = bletags.begin(); itags != bletags.end(); ++itags) {
    if((*itags).mac == mac) {
      if(((xTaskGetTickCount() - (*itags).lastseen) / 1000) > timeout) {
        client->println(msg);
        break;
      } else {
        msg = F("present;device_name=");
        msg += (*itags).tname.c_str();
        msg += F(";rssi=");
        int nrssi = 0;
        std::deque<int>::iterator irssi;
        while(updating) delay(10);
        for(irssi = (*itags).rssi.begin();irssi != (*itags).rssi.end(); ++irssi) {
          nrssi += (*irssi);
        }
        nrssi /= static_cast<int>((*itags).rssi.size());
        msg += String(nrssi);
        msg += F(";daemon=BLEScanner V0.1");
        client->println(msg.c_str());
        Serial.print(F("Sending >"));
        Serial.print(msg.c_str());
        Serial.print(F("< to "));
        Serial.print(client->remoteIP().toString());
        Serial.print(F(":"));
        Serial.println(client->remotePort());
      }
      return;
    }
  }
  client->println(F("absence;rssi=unreachable;daemon=BLEScanner V0.1"));
}



void handleClient(void * pvParameters)
{
  char buf[64];
  unsigned char c[2];
  std::string cmac = "";
  short timeout = 300;
  uint32_t lastreport;
  short pos = 0;

  WiFiClient* client = static_cast<WiFiClient*>(pvParameters);  
  while (client->connected()) {
    if(client->available()) {
      if(client->read(c, 1) == 1)
      {
        buf[pos] = c[0];
        if(buf[pos] == '\n' || pos == 63) {
          buf[pos] = '\0';
          Serial.print(F("Request >"));
          Serial.print(buf);
          Serial.print(F("< from "));
          Serial.print(client->remoteIP().toString());
          Serial.print(":");
          Serial.println(client->remotePort());
          if(pos > 18) {
            cmac.assign(buf);
            cmac.erase(17, strlen(buf) - 17);
            std::transform(cmac.begin(), cmac.end(), cmac.begin(), [](unsigned char c) -> unsigned char { return std::tolower(c); });
            timeout = atoi(buf + 18);
          } else if(strcmp(buf, "now") == 0) {
            if(cmac.length() > 0) {
              printTags(client, cmac, timeout);
              lastreport = xTaskGetTickCount();
            }
          } else if(strcmp(buf, "DEBUG") == 0) {
            esp_log_level_set("*", ESP_LOG_DEBUG);
          } else if(strcmp(buf, "NODEBUG") == 0) {
            esp_log_level_set("*", ESP_LOG_NONE);
          }
          pos = 0;
        } else {
          ++pos;
        }
      }
    } else {
      if((xTaskGetTickCount() - lastreport) / 1000 >= timeout) {
        if(cmac.length() > 0) {
          printTags(client, cmac, timeout);
          lastreport = xTaskGetTickCount();
        }
      }
      delay(100);
    }
  }
  client->stop();
  delete(client);
  vTaskDelete(NULL);
}

void wifiTask(void * pvParameters)
{
  if (useStaticIP) {
    WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  }
  WiFi.onEvent(WiFiEvent);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(50);
  }

  Serial.println(F("\nWiFi connected."));
  Serial.println(F("IP address: "));
  Serial.println(WiFi.localIP());
  server.begin();

  char param[20];
  char buf[64];
  int pbuf = 0;
  uint32_t lcnt = 0;

  for (;;) {
    WiFiClient* client = new WiFiClient();
    for(;;) {
      *client = server.available();
      if(*client) {
        Serial.print(F("New connection from "));
        Serial.print(client->remoteIP().toString());
        Serial.print(":");
        Serial.println(client->remotePort());
        // xTaskCreatePinnedToCore(handleClient, "clientTask", 2000, client, 1, NULL, 1);
        xTaskCreate(handleClient, "clientTask", 2000, client, 1, NULL);
        break;
      } else {
        delay(100);
      }
    }
  }
}

void verbose_print_reset_reason(RESET_REASON reason)
{
  switch ( reason)
  {
    case 1  : Serial.println (F("Vbat power on reset"));break;
    case 3  : Serial.println (F("Software reset digital core"));break;
    case 4  : Serial.println (F("Legacy watch dog reset digital core"));break;
    case 5  : Serial.println (F("Deep Sleep reset digital core"));break;
    case 6  : Serial.println (F("Reset by SLC module, reset digital core"));break;
    case 7  : Serial.println (F("Timer Group0 Watch dog reset digital core"));break;
    case 8  : Serial.println (F("Timer Group1 Watch dog reset digital core"));break;
    case 9  : Serial.println (F("RTC Watch dog Reset digital core"));break;
    case 10 : Serial.println (F("Instrusion tested to reset CPU"));break;
    case 11 : Serial.println (F("Time Group reset CPU"));break;
    case 12 : Serial.println (F("Software reset CPU"));break;
    case 13 : Serial.println (F("RTC Watch dog Reset CPU"));break;
    case 14 : Serial.println (F("for APP CPU, reseted by PRO CPU"));break;
    case 15 : Serial.println (F("Reset when the vdd voltage is not stable"));break;
    case 16 : Serial.println (F("RTC Watch dog reset digital core and rtc module"));break;
    default : Serial.println (F("NO_MEAN"));
  }
}


void setup()
{
  esp_log_level_set("*", ESP_LOG_NONE);
  Serial.begin(115200);
  delay(2000);

  Serial.print(F("CPU0 reset reason: "));
  verbose_print_reset_reason(rtc_get_reset_reason(0));

  Serial.print(F("CPU1 reset reason: "));
  verbose_print_reset_reason(rtc_get_reset_reason(1));

  xTaskCreatePinnedToCore(bleTask, "bleTask", 20000, NULL, 0, NULL, 0);
  xTaskCreatePinnedToCore(wifiTask, "wifiTask", 5000, NULL, 1, NULL, 1);
}

void loop() {
}
