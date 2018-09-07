/********************************************************************************
* 
* BLEScanner by micky0867
* 
* For use with PRESENCE modul in FHEM 
* A cheap solution for (multiple) BLE detectors
* 
* History
* 
* V0.1 (06.01.2018)
*   Initial version
* 
* V0.2 (11.08.2018)
*   Use watchdog to recover from hanging bleTask
*   Count uptime in seconds (nearly)
*   Fastdetection to re-detect Tags within seconds
*   
* V0.3 (07.09.2018)
*   New watchdog strategy
* 
********************************************************************************/
#define _USEWATCHDOG_ 1
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
#include <map>
#include <algorithm>

const char* ssid     = "ENTER_YOUR_WIFI_SSID_HERE";
const char* password = "ENTER_YOUR_WIFI_PASSWORD_HERE";
const char* hostname = "ENTER_THE_HOSTNAME_HERE";

//scanduration in seconds
int scanTime = 8; 

// Timeout:
// Not yet requested Tags: Remove from list if not seen for n seconds
// Already requested Tags: Invalidate RSSI data for faster re-detection
uint32_t tagtimeout = 180;

bool useStaticIP = false;
IPAddress local_IP(192, 168, 178, 78);
IPAddress gateway(192, 168, 178, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 178, 1); //optional
IPAddress secondaryDNS(8, 8, 8, 8); //optional
WiFiServer server(3333, 16);  // Port, Maxclients



static BLEClient* pClient;
static BLEAddress* pAddress;
static BLERemoteService* pRemoteService;
static BLERemoteCharacteristic* pRemoteCharacteristic;

TaskHandle_t h_bletask;
int reconnects = -1;
int restarts = 0;
bool wifiConnect = false;
double tickspersec = pdMS_TO_TICKS(1000);
long uptimesec = 0;
long blealivesec = 0;
boolean blockble = false;      // block ble-task for testing
boolean blockforever = false;  // keep ble-task blocked

SemaphoreHandle_t  xMutexBleTags = xSemaphoreCreateMutex( );
SemaphoreHandle_t  xMutexScan = xSemaphoreCreateMutex( );

struct BLETag {
  std::string mac;
  long lastseen;
  std::string tname;
  std::deque<int> rssi;
};

std::list<BLETag> bletags;
std::list<BLETag>::iterator itags;

std::map<TaskHandle_t, std::string> tasks;
std::map<std::string, short> fastdetection;


class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks
{
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      xSemaphoreTake(xMutexBleTags, portMAX_DELAY);
      for (itags = bletags.begin(); itags != bletags.end(); ++itags) {
        if((*itags).mac == advertisedDevice.getAddress().toString()) {
          (*itags).lastseen = uptimesec;
          if (advertisedDevice.getName().length() > 0 &&
              advertisedDevice.getName() != (*itags).tname) {
            (*itags).tname = advertisedDevice.getName();
          }
          
          (*itags).rssi.push_back(advertisedDevice.getRSSI());
          if((*itags).rssi.size() > 5)
            (*itags).rssi.pop_front();
          if((*itags).rssi.size() == 1 && fastdetection.find((*itags).mac) != fastdetection.end() && fastdetection[(*itags).mac] == 0) {
            fastdetection[(*itags).mac] = 1;
          }
          xSemaphoreGive(xMutexBleTags);
          return;
        }
      }
      std::string nmac = advertisedDevice.getAddress().toString();
      std::transform(nmac.begin(), nmac.end(), nmac.begin(), ::tolower);
      bletags.push_back(BLETag{nmac,
                               uptimesec,
                               advertisedDevice.getName(),
                               std::deque<int>{advertisedDevice.getRSSI()}});
      xSemaphoreGive(xMutexBleTags);
    }
};


void cleanupTags()
{
  xSemaphoreTake(xMutexBleTags, portMAX_DELAY);
  for (itags = bletags.begin(); itags != bletags.end();) {
    // if(xTaskGetTickCount() < (*itags).lastseen) // uptime counter overflow
    //  (*itags).lastseen = xTaskGetTickCount();
    if((uptimesec - (*itags).lastseen) > tagtimeout) {
      if(fastdetection.find((*itags).mac) != fastdetection.end()) {
        (*itags).rssi.clear();
        ++itags;
      } else {
        bletags.erase(itags++);
      }
    } else {
      ++itags;
    }
  }
  xSemaphoreGive(xMutexBleTags);
}


void bleTask(void * pvParameters)
{
#ifdef _USEWATCHDOG_
  esp_task_wdt_add(NULL);
#endif

  Serial.println(F("Starting BLE init"));
  BLEDevice::init("");
  BLEScan* pBLEScan;
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  for (;;) {
    while(blockble) {
      // loop for watchdog test
    }
    blealivesec = uptimesec;
#ifdef _USEWATCHDOG_
    esp_task_wdt_reset();
#endif
    while(xSemaphoreTake(xMutexScan, scanTime * tickspersec) == pdFALSE) {
#ifdef _USEWATCHDOG_
      esp_task_wdt_reset();
#endif
      blealivesec = uptimesec;
      delay(100);
    }
    Serial.println("Scanning ...");
    BLEScanResults foundDevices = pBLEScan->start(scanTime);
    xSemaphoreGive(xMutexScan);
    delay(1000);
    cleanupTags();
    delay(100);
  }
}


void WiFiEvent(WiFiEvent_t event)
{
  switch (event) {
    case SYSTEM_EVENT_STA_START:
      Serial.println(F("SYSTEM_EVENT_STA_START"));
      WiFi.setHostname(hostname);
      break;
    case SYSTEM_EVENT_STA_CONNECTED:
      Serial.println(F("SYSTEM_EVENT_STA_CONNECTED"));
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println(F("SYSTEM_EVENT_STA_DISCONNECTED"));
      if(wifiConnect)
        xSemaphoreTake(xMutexScan, portMAX_DELAY);
      wifiConnect = false;
      WiFi.reconnect();
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.println(F("SYSTEM_EVENT_STA_GOT_IP"));
      wifiConnect = true;
      xSemaphoreGive(xMutexScan);
      ++reconnects;
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
  xSemaphoreTake(xMutexBleTags, portMAX_DELAY);
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
  xSemaphoreGive(xMutexBleTags);
  Serial.println("");
}

void printTags(WiFiClient* client, std::string mac, short timeout, String reason)
{
  String msg = F("absence;rssi=unreachable;daemon=BLEScanner V0.3");
  xSemaphoreTake(xMutexBleTags, portMAX_DELAY);
  for (itags = bletags.begin(); itags != bletags.end(); ++itags) {
    if((*itags).mac == mac) {
      if(((uptimesec - (*itags).lastseen)) > timeout) {
        client->println(msg);
        break;
      } else {
        msg = F("present;device_name=");
        msg += (*itags).tname.c_str();
        msg += F(";rssi=");
        int nrssi = 0;
        std::deque<int>::iterator irssi;
        for(irssi = (*itags).rssi.begin();irssi != (*itags).rssi.end(); ++irssi) {
          nrssi += (*irssi);
        }
        nrssi /= static_cast<int>((*itags).rssi.size());
        msg += String(nrssi);
        msg += F(";daemon=BLEScanner V0.2");
        client->println(msg.c_str());
        Serial.print(F("Sending >"));
        Serial.print(msg.c_str());
        Serial.print(F("< to "));
        Serial.print(client->remoteIP().toString());
        Serial.print(F(":"));
        Serial.print(client->remotePort());
        Serial.print(F(" for tag "));
        Serial.print(mac.c_str());
        Serial.print(F(", Reason: "));
        Serial.println(reason);
      }
      xSemaphoreGive(xMutexBleTags);
      return;
    }
  }
  xSemaphoreGive(xMutexBleTags);
  client->println(msg);
  Serial.print(F("Sending >"));
  Serial.print(msg.c_str());
  Serial.print(F("< to "));
  Serial.print(client->remoteIP().toString());
  Serial.print(F(":"));
  Serial.print(client->remotePort());
  Serial.print(F(" for tag "));
  Serial.print(mac.c_str());
  Serial.print(F(", Reason: "));
  Serial.println(reason);
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
            fastdetection[cmac] = 0;
            printTags(client, cmac, timeout, "on request");
            lastreport = uptimesec;
          } else if(strcmp(buf, "now") == 0) {
            if(cmac.length() > 0) {
              printTags(client, cmac, timeout, "forced request");
              lastreport = uptimesec;
            }
          } else if(strcmp(buf, "DEBUG") == 0) {
            esp_log_level_set("*", ESP_LOG_DEBUG);
          } else if(strcmp(buf, "NODEBUG") == 0) {
            esp_log_level_set("*", ESP_LOG_NONE);
          } else if(strcmp(buf, "BLOCKBLE") == 0) {
            Serial.println("BLE Task blocked!");
            blockble = true;
          } else if(strcmp(buf, "BLOCKFOREVER") == 0) {
            Serial.println("BLE Task blocked forever!");
            blockble = true;
            blockforever = true;
          }
          pos = 0;
        } else {
          ++pos;
        }
      }
    } else {
      if(cmac.length() > 0 && (uptimesec - lastreport) >= timeout) {
        printTags(client, cmac, timeout, "periodic report");
        lastreport = uptimesec;
      } else if(cmac.length() > 0 && fastdetection[cmac] == 1) {
        printTags(client, cmac, timeout, "fastdetection");
        lastreport = uptimesec;
        delay(1000); 
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
    delay(100);
  }

#ifdef _USEWATCHDOG_
  esp_task_wdt_add(NULL);
#endif
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
  #ifdef _USEWATCHDOG_
      esp_task_wdt_reset();
  #endif
      *client = server.available();
      if(*client) {
        Serial.print(F("New connection from "));
        Serial.print(client->remoteIP().toString());
        Serial.print(":");
        Serial.println(client->remotePort());

        TaskHandle_t h_task;
        String strremotePort(client->remotePort());
        std::string tname = "clientTask_";
        tname += client->remoteIP().toString().c_str();
        tname += ":";
        tname += strremotePort.c_str();
        xTaskCreate(handleClient, tname.c_str(), 2000, client, 2, &h_task);
        tasks[h_task] = tname;
        break;
      } else {
        delay(500);
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
  Serial.begin(57600);
  delay(2260);
  uptimesec = 2;

  Serial.print(F("CPU0 reset reason: "));
  verbose_print_reset_reason(rtc_get_reset_reason(0));

  Serial.print(F("CPU1 reset reason: "));
  verbose_print_reset_reason(rtc_get_reset_reason(1));

  TaskHandle_t h_task;
  
  xTaskCreatePinnedToCore(bleTask, "bleTask", 2500, NULL, 3, &h_bletask, 0);
  tasks[h_bletask] = "bleTask";
  xSemaphoreTake(xMutexScan, portMAX_DELAY); // will be released by Wifi-Connection-State
  xTaskCreatePinnedToCore(wifiTask, "wifiTask", 3000, NULL, 1 | portPRIVILEGE_BIT, &h_task, 1);
  tasks[h_task] = "wifiTask";
  Serial.print("Ticks per second: ");
  Serial.println(tickspersec);
#ifdef _USEWATCHDOG_ 
  esp_task_wdt_init(scanTime * 4, true);
#endif
}

void loop() {
  for(short i=0; i<60; ++i) {
    delay(tickspersec);
    ++uptimesec;
#ifdef _USEWATCHDOG_
    // detect hanging BLE-Taks, before it is detected by esp_watchdog
    if(uptimesec - blealivesec > scanTime * 2  && !blockforever) {
      Serial.print("BLE-Task is hanging for ");
      Serial.print(uptimesec - blealivesec);
      Serial.println(" seconds");
      esp_task_wdt_delete(h_bletask);
      vTaskDelete(h_bletask);
      tasks.erase(h_bletask);
      delay(tickspersec);
      ++uptimesec;
      Serial.println("Restarting BLE-Task");
      xTaskCreatePinnedToCore(bleTask, "bleTask", 2500, NULL, 3, &h_bletask, 0);
      tasks[h_bletask] = "bleTask";
      blockble = false;
      ++restarts;
    }
#endif
  }

  Serial.print(F("Free heap: "));
  Serial.println(esp_get_free_heap_size());
  // delay(100);
  std::map<TaskHandle_t, std::string>::iterator ittasks;
  for(ittasks = tasks.begin(); ittasks != tasks.end();)
  {
    if(eTaskGetState((*ittasks).first) == eReady && // should be eDeleted?
       (*ittasks).second.substr(0, 6) == "client") { 
      tasks.erase(ittasks++);
    } else {
      Serial.print("Task ");
      Serial.print(((*ittasks).second).c_str());
      Serial.print(" state: ");
      Serial.print(eTaskGetState((*ittasks).first));
      Serial.print(" free stack: ");
      Serial.println(uxTaskGetStackHighWaterMark((*ittasks).first));
      ++ittasks;
    }
  }
  Serial.println("Tags for faster detection:");
  for(const auto& pair : fastdetection) {
    Serial.print("  >");
    Serial.println(pair.first.c_str());
  }
  if(fastdetection.size() == 0)
    Serial.println("  >none");
  // delay(100);
  Serial.print("Uptime: ");
  Serial.print(uptimesec);
  Serial.println(" seconds");
  // delay(100);
  dumpTags();
  // delay(100);
  Serial.print("Reconnects:   ");
  Serial.println(reconnects);
  Serial.print("BLE-Restarts: ");
  Serial.println(restarts);
  Serial.print("Uptime: ");
  Serial.println(uptimesec);
}

