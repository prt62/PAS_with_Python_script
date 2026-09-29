#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h> // Provides WiFiServer and WiFiClient for native Ethernet
#include <SD_MMC.h>
#include <Preferences.h>
#include "esp_mac.h"
#include "esp_bt.h"

// --- BLE Libraries ---
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// --- Audio Libraries ---
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <AudioFileSourceFS.h>     // <--- Changed to support SD_MMC!
#include <AudioFileSourceBuffer.h> // <--- 16KB RAM Buffer included natively!

#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <AudioFileSourceFS.h>
#include <AudioFileSourceID3.h> // <--- 1. Make sure this include is here!

// ... (your other network/SD includes) ...

// --- AUDIO POINTERS ---
//AudioGeneratorMP3 *mp3;
//AudioFileSourceFS *file;
AudioFileSourceID3 *id3 = nullptr; // <--- 2. THIS IS THE MISSING DECLARATION!
//AudioOutputI2S *out;

// (Notice that 'buff' is completely gone from this list, which is correct!)

// --- Olimex ESP32-POE Native Ethernet Pins ---
#define ETH_PHY_ADDR  0
#define ETH_PHY_POWER 12
#define ETH_PHY_MDC   23
#define ETH_PHY_MDIO  18
#define ETH_PHY_TYPE  ETH_PHY_LAN8720
#define ETH_CLK_MODE  ETH_CLOCK_GPIO17_OUT

// --- Safe Audio Pins (Olimex EXT1 Header) ---
#define I2S_LRC   32
#define I2S_BCLK  33
#define I2S_DOUT  16

#define STATUS_LED 5 // Safely moved off internal SD/ETH pins

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

byte mac[] = { 0x02, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5 };



WiFiServer *server = nullptr; // Converted to pointer for dynamic port assignment
int currentPort;              // Stores the active listening port
Preferences preferences;
//WiFiServer server(5000);   // <--- Changed from EthernetServer
//Preferences preferences;

AudioGeneratorMP3 *mp3;
AudioFileSourceFS *file;
AudioOutputI2S *out;
AudioFileSourceBuffer *buff = nullptr;

String currentIP;
String currentGateway; // <--- ADD THIS NEW LINE
bool hardwareError = false; 
bool sdCardOK = false;

BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;

// ==========================================
// ---> DUAL-CORE COMMUNICATION FLAGS
// ==========================================
volatile bool reqPlay = false;
volatile bool reqStop = false;
volatile bool isAudioPlaying = false; 

// Task Handle for Core 0
TaskHandle_t NetworkTaskHandle;


// --- BLE Callbacks ---
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { deviceConnected = true; };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      BLEDevice::startAdvertising(); 
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String rxValue = pCharacteristic->getValue().c_str(); 
      rxValue.trim();
      
      if (rxValue.length() > 0) {
        Serial.println("\n[BLE] Received Raw: " + rxValue);
        
        // Check for exact format: *$101,IP,PORT#
        if (rxValue.startsWith("*$101,") && rxValue.endsWith("#")) {
          
          // Strip away "*$101," from the front, and "#" from the back
          String payload = rxValue.substring(6, rxValue.length() - 1); 
          
          int commaIndex = payload.indexOf(',');
          if (commaIndex > 0) {
            String newIP = payload.substring(0, commaIndex);
            String portStr = payload.substring(commaIndex + 1);
            int newPort = portStr.toInt();

            if (newPort > 0 && newPort <= 65535) {
              preferences.begin("network", false);
              preferences.putString("saved_ip", newIP);
              preferences.putInt("saved_port", newPort);
              preferences.end();
              
              Serial.print("[BLE] IP/Port Saved! Re-booting into: ");
              Serial.println(newIP + ":" + String(newPort));
              delay(3000);
              ESP.restart(); 
            } else {
              Serial.println("[BLE] ERROR: Invalid Port Number.");
            }
          }
        } else {
          Serial.println("[BLE] ERROR: Invalid Command Format.");
        }
      }
    }
};

#if 0
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      
      // ---> THE FIX: Add .c_str() to convert to an Arduino String <---
      String rxValue = pCharacteristic->getValue().c_str(); 
      
      if (rxValue.length() > 0) {
        String newIP = rxValue; 
        newIP.trim(); 
        Serial.println("\n[BLE] Received New IP Command: " + newIP);
        
        preferences.begin("network", false);
        preferences.putString("saved_ip", newIP);
        preferences.end();
        
        Serial.println("[BLE] IP Saved! Rebooting in 3 seconds to apply...");
        delay(3000);
        ESP.restart(); 
      }
    }
};
#endif

// class MyCallbacks: public BLECharacteristicCallbacks {
//     void onWrite(BLECharacteristic *pCharacteristic) {
//       String rxValue = pCharacteristic->getValue(); 
//       if (rxValue.length() > 0) {
//         String newIP = rxValue; 
//         newIP.trim(); 
//         Serial.println("\n[BLE] Received New IP Command: " + newIP);
        
//         preferences.begin("network", false);
//         preferences.putString("saved_ip", newIP);
//         preferences.end();
        
//         Serial.println("[BLE] IP Saved! Rebooting in 3 seconds to apply...");
//         delay(3000);
//         ESP.restart(); 
//       }
//     }
// };

// void stopAudio() {
//   if (mp3->isRunning()) mp3->stop();
//   if (buff) { buff->close(); delete buff; buff = nullptr; } // Clean buffer
//   if (file->isOpen()) file->close();
//   isAudioPlaying = false;
// }

void stopAudio() {
  if (mp3->isRunning()) mp3->stop();
  
  if (id3)  { id3->close();  delete id3;  id3 = nullptr;  } 
  if (file->isOpen()) file->close();
  
  isAudioPlaying = false;
}


// ==========================================
// CORE 0 - NETWORK & PROTOCOL TASK
// ==========================================
void networkTaskCode(void * parameter) {
  for(;;) {
    // 1. Maintain Network & Diagnostics
    static unsigned long lastDiagnostic = 0;
    static bool previousLinkState = true; 
    
    // ---> THE FIX: Add the Watchdog Counter <---
    static int ethDisconnectCounter = 0; 

    if (millis() - lastDiagnostic > 2000) {
      lastDiagnostic = millis();
      bool linkUp = ETH.linkUp(); 
      
      // --- LED Updates ---
      if (hardwareError) {
        digitalWrite(STATUS_LED, !digitalRead(STATUS_LED)); 
      } else if (!linkUp) {
        digitalWrite(STATUS_LED, HIGH); 
        vTaskDelay(100 / portTICK_PERIOD_MS);
        digitalWrite(STATUS_LED, LOW);
      } else {
        digitalWrite(STATUS_LED, HIGH); 
      }

      // ---> THE ETHERNET WATCHDOG LOGIC <---
      if (!linkUp) {
        ethDisconnectCounter++;
        Serial.print("[Network] Warning: Ethernet Link Down. Watchdog: ");
        Serial.print(ethDisconnectCounter);
        Serial.println(" / 1500");
        
        // If the link is dead for 1500 consecutive checks (30 seconds)
        if (ethDisconnectCounter >= 1500) {
          Serial.println("\n[FATAL] Ethernet failed to recover automatically!");
          Serial.println(">>> REBOOTING SYSTEM TO RESET HARDWARE PHY <<<\n");
          
          // Stop audio to prevent memory spikes during shutdown
          reqStop = true; 
          vTaskDelay(1000 / portTICK_PERIOD_MS); 
          
          ESP.restart(); // Hard reset the microcontroller and EMAC
        }
      } else {
        // If the cable is plugged back in or recovers, reset the countdown!
        ethDisconnectCounter = 0; 
      }
  
      // --- BLE Updates ---
      if (pCharacteristic) {
        
        // ---> THE HEAP FRAGMENTATION FIX <---
        // Replace memory-shredding String math with a clean, static C-buffer!
        char telemetry[128];
        // Show IP:PORT in the BLE app
        snprintf(telemetry, sizeof(telemetry), "%s:%d | GW:%s | %s | SD:%s\n", 
                 currentIP.c_str(), 
                 currentPort,         // <--- Added Port Variable
                 currentGateway.c_str(), 
                 linkUp ? "LNK:UP" : "LNK:DOWN", 
                 sdCardOK ? "OK" : "FAIL");
        
        pCharacteristic->setValue(telemetry);
        if (deviceConnected) pCharacteristic->notify();
      }
      
      if (linkUp != previousLinkState) {
        if (linkUp) Serial.println("\n[Network] LINK RESTORED: Ethernet cable connected.");
        else Serial.println("\n[Network] WARNING: Ethernet cable UNPLUGGED!");
        previousLinkState = linkUp; 
      }
    }

// 2. Handle TCP Commands
    WiFiClient client = server->available(); 
    if (client) {
      client.setNoDelay(true); // Fire packets instantly
      unsigned long timeout = millis();
      
      // Patient listener: Wait for exactly 4 bytes
      while(client.available() < 4 && (millis() - timeout < 1000)) { 
        vTaskDelay(1); 
      }

      if (client.available() >= 4) {
        char cmd[5] = {0}; 
        client.read((uint8_t*)cmd, 4);
        
        // Instantly clear out any hidden characters like \r\n
        while(client.available()) { client.read(); } 

        if (strcmp(cmd, "STOP") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: STOP");
          
          // 1. Send the ACK while memory is stable
          client.print("ACK"); 
          
          // 2. Give the hardware 20ms to push the packet into the cable
          vTaskDelay(20 / portTICK_PERIOD_MS); 
          
          // 3. Cleanly sever the network connection
          client.stop(); 

          // ---> THE MEMORY FIX: Save state to Flash <---
          preferences.begin("audio", false);
          preferences.putBool("auto_play", false);
          preferences.end();
          
          // 4. NOW tear down the audio memory! (No bus collisions!)
          reqStop = true; 
        }
        else if (strcmp(cmd, "PLAY") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: PLAY");
          
          client.print("ACK"); 
          vTaskDelay(20 / portTICK_PERIOD_MS); 
          client.stop(); 

          // ---> THE MEMORY FIX: Save state to Flash <---
          preferences.begin("audio", false); // Open in read/write mode
          preferences.putBool("auto_play", true);
          preferences.end();
          
          reqPlay = true; // NOW start the audio
        }
        else if (strcmp(cmd, "UPLD") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: UPLOAD");
          
          // Audio MUST be stopped first here so we can write to the SD card
          reqStop = true; 
          while(reqStop || isAudioPlaying) { vTaskDelay(10); } 
          
          if (SD_MMC.exists("/song.mp3")) SD_MMC.remove("/song.mp3");
          File dataFile = SD_MMC.open("/song.mp3", FILE_WRITE);
          
          if (dataFile) {
            uint32_t totalBytes = 0;
            uint32_t lastDataTime = millis();
            while (client.connected()) {
              if (client.available()) {
                uint8_t buffer[4096]; 
                int bytesRead = client.read(buffer, sizeof(buffer));
                if (bytesRead > 0) {
                  dataFile.write(buffer, bytesRead); 
                  totalBytes += bytesRead;
                  lastDataTime = millis();
                }
              } else {
                if (millis() - lastDataTime > 15000) break; 
              }
              vTaskDelay(1); 
            }
            dataFile.close();
            Serial.println("[Core 0] [+] Upload Complete.");
          }
          client.stop();
        }
      } else {
        // If it wasn't a valid command, close it cleanly
        client.stop(); 
      }
    }
#if 0
// 2. Handle TCP Commands
    WiFiClient client = server.available(); 
    if (client) {
      
      // ---> THE ULTIMATE FIX: Disable Nagle's Algorithm <---
      // This forces the ESP32 to transmit the 3-byte "ACK" instantly 
      // instead of holding it hostage in RAM, completely breaking the deadlock!
      client.setNoDelay(true);
      client.setTimeout(1); 
      
      unsigned long timeout = millis();
      
      // Patient listener: Wait for exactly 4 bytes
      while(client.available() < 4 && (millis() - timeout < 1000)) { 
        vTaskDelay(1); 
      }

      if (client.available() >= 4) {
        char cmd[5] = {0}; 
        client.read((uint8_t*)cmd, 4);

        if (strcmp(cmd, "UPLD") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: UPLOAD");
          reqStop = true; 
          while(reqStop || isAudioPlaying) { vTaskDelay(10); } 
          
          if (SD_MMC.exists("/song.mp3")) SD_MMC.remove("/song.mp3");
          File dataFile = SD_MMC.open("/song.mp3", FILE_WRITE);
          if (dataFile) {
            uint32_t totalBytes = 0;
            uint32_t lastDataTime = millis();
            while (client.connected()) {
              if (client.available()) {
                uint8_t buffer[4096]; 
                int bytesRead = client.read(buffer, sizeof(buffer));
                if (bytesRead > 0) {
                  dataFile.write(buffer, bytesRead); 
                  totalBytes += bytesRead;
                  lastDataTime = millis();
                }
              } else {
                if (millis() - lastDataTime > 15000) break; 
              }
              vTaskDelay(1); 
            }
            dataFile.close();
            Serial.println("[Core 0] [+] Upload Complete.");
          }
        }
        else if (strcmp(cmd, "PLAY") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: PLAY");
          
          client.print("ACK"); // Because of NoDelay, this fires instantly!
          reqPlay = true; 
        }
        else if (strcmp(cmd, "STOP") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: STOP");
          
          client.print("ACK"); // Because of NoDelay, this fires instantly!
          reqStop = true; 
        }
      }
      
      // Clear out any hidden trailing characters
      while(client.available()) { client.read(); }

      // Give the hardware a tiny fraction of a second to physically push the packet out
      vTaskDelay(10 / portTICK_PERIOD_MS); 
      
      client.stop(); // Safely kill the empty socket 
    }
    // 2. Handle TCP Commands
    WiFiClient client = server.available(); 
    if (client) {
      
      // THE FIX: Set a strict 1-second timeout so the ESP32 NEVER hangs for 10 seconds!
      client.setTimeout(1); 
      
      unsigned long timeout = millis();
      
      // Patient listener: Wait for exactly 4 bytes
      while(client.available() < 4 && (millis() - timeout < 1000)) { 
        vTaskDelay(1); 
      }

      if (client.available() >= 4) {
        char cmd[5] = {0}; 
        client.read((uint8_t*)cmd, 4);

        if (strcmp(cmd, "UPLD") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: UPLOAD");
          reqStop = true; 
          while(reqStop || isAudioPlaying) { vTaskDelay(10); } 
          
          if (SD_MMC.exists("/song.mp3")) SD_MMC.remove("/song.mp3");
          File dataFile = SD_MMC.open("/song.mp3", FILE_WRITE);
          if (dataFile) {
            uint32_t totalBytes = 0;
            uint32_t lastDataTime = millis();
            while (client.connected()) {
              if (client.available()) {
                uint8_t buffer[4096]; 
                int bytesRead = client.read(buffer, sizeof(buffer));
                if (bytesRead > 0) {
                  dataFile.write(buffer, bytesRead); 
                  totalBytes += bytesRead;
                  lastDataTime = millis();
                }
              } else {
                if (millis() - lastDataTime > 15000) break; 
              }
              vTaskDelay(1); 
            }
            dataFile.close();
            Serial.println("[Core 0] [+] Upload Complete.");
          }
        }
        else if (strcmp(cmd, "PLAY") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: PLAY");
          reqPlay = true; 
          
          client.print("ACK");
          vTaskDelay(10); // <--- THE FIX: Give LwIP hardware 10ms to push the packet out!
        }
        else if (strcmp(cmd, "STOP") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: STOP");
          reqStop = true; 
          
          client.print("ACK");
          vTaskDelay(10); // <--- THE FIX: Give LwIP hardware 10ms to push the packet out!
        }
      }
      
      // Clear out any trailing hidden characters
      while(client.available()) { client.read(); }

      vTaskDelay(2);
      client.stop(); // Safely kill the socket 
    }
    // 2. Handle TCP Commands
    WiFiClient client = server.available(); // <--- Changed from EthernetClient
    if (client) {
      unsigned long timeout = millis();
      
      while(client.available() < 4 && (millis() - timeout < 1000)) { 
        vTaskDelay(1); 
      }

      if (client.available() >= 4) {
        char cmd[5] = {0}; 
        client.read((uint8_t*)cmd, 4);

        if (client.peek() == '\n') client.read(); 

        if (strcmp(cmd, "UPLD") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: UPLOAD");
          
          reqStop = true; 
          while(reqStop || isAudioPlaying) { vTaskDelay(10); } 
          
          if (SD_MMC.exists("/song.mp3")) SD_MMC.remove("/song.mp3");
          
          File dataFile = SD_MMC.open("/song.mp3", FILE_WRITE);
          if (dataFile) {
            uint32_t totalBytes = 0;
            uint32_t lastPrint = 0;
            uint32_t lastDataTime = millis();
            
            while (client.connected()) {
              if (client.available()) {
                uint8_t buffer[4096]; // Increased block size for SDMMC speed!
                int bytesRead = client.read(buffer, sizeof(buffer));
                if (bytesRead > 0) {
                  dataFile.write(buffer, bytesRead); 
                  totalBytes += bytesRead;
                  lastDataTime = millis();
                }
                
                if (totalBytes - lastPrint >= 102400) {
                  Serial.print("      Saved: ");
                  Serial.print(totalBytes / 1024);
                  Serial.println(" KB");
                  lastPrint = totalBytes;
                }
              } else {
                if (millis() - lastDataTime > 15000) { 
                  Serial.println("\n[Core 0] [ERROR] Upload stream timed out! Aborting.");
                  break; 
                }
              }
              vTaskDelay(1); 
            }
            
            dataFile.close();
            Serial.print("[Core 0] [+] Upload Complete. Total Size: ");
            Serial.print(totalBytes / 1024);
            Serial.println(" KB");
          }
        }
        else if (strcmp(cmd, "PLAY") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: PLAY");
          client.print("ACK");
          reqPlay = true; 
        }
        else if (strcmp(cmd, "STOP") == 0) {
          Serial.println("\n[Core 0] [>>>] COMMAND: STOP");
          client.print("ACK");
          reqStop = true; 
        }
      }
      
     // client.flush();
      vTaskDelay(5);
      client.stop(); 
    }
  #endif
    vTaskDelay(5 / portTICK_PERIOD_MS); 
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW); 

  Serial.println("\n==================================================");
  Serial.println("--- OLIMEX ESP32-POE AUDIO PLAYER ---");
  Serial.println("==================================================");

  preferences.begin("network", false);
  currentIP = preferences.getString("saved_ip", "192.168.5.50"); 

  // ---> NEW: Load the saved port (Defaults to 5000 on first boot)
  currentPort = preferences.getInt("saved_port", 5000);

  preferences.end();
  
  IPAddress ip;
  ip.fromString(currentIP);

  // ---> THE DYNAMIC GATEWAY FIX <---
  // Take the first 3 octets of the assigned IP, and set the 4th octet to '1'
  IPAddress gateway(ip[0], ip[1], ip[2], 1); 
  currentGateway = gateway.toString(); // Save as string for BLE broadcast

  IPAddress dns(8, 8, 8, 8); 
  IPAddress subnet(255, 255, 255, 0);

  // --- BLE Setup ---
  uint8_t mac_addr[6];
  esp_read_mac(mac_addr, ESP_MAC_WIFI_STA); 
  char bleName[32];
  sprintf(bleName, "AJEEVI_PAS_%02X%02X%02X", mac_addr[3], mac_addr[4], mac_addr[5]);

  // Release Classic BT Memory (~30KB saved!)
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT); // <--- ADD THIS
  
  BLEDevice::init(bleName);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE  |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pCharacteristic->addDescriptor(new BLE2902());
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->setValue("BOOTING..."); 
  pService->start();
  BLEDevice::getAdvertising()->start();

  // --- Initialize High-Speed SDMMC ---
  Serial.print("[Hardware] Mounting Native SD_MMC (1-Bit Mode)... ");
  // CRITICAL: Must pass 'true' to force 1-bit mode, otherwise it conflicts with Ethernet!

  if(SD_MMC.begin("/sdcard", true,false,20000)) {
    sdCardOK = true;
    Serial.println("OK.");
    Serial.print("[Hardware] SD Size: ");
    Serial.print(SD_MMC.totalBytes() / (1024 * 1024)); Serial.println(" MB");
  } else {
    hardwareError = true;
    Serial.println("FAILED! Ensure card is inserted.");
  }
  
  // --- Initialize Native Ethernet ---
  Serial.print("[Network] Initializing LAN8720 RMII Ethernet... ");
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE);
  ETH.config(ip, gateway, subnet, dns);
  Serial.println("OK.");

  Serial.println("\n--------------------------------------------------");
  Serial.print(">>> CRITICAL NATIVE ETH STATE <<<");
  Serial.print("\n    Internal IP: "); Serial.println(ETH.localIP());
  Serial.print("    Link Status: "); 
  Serial.println(ETH.linkUp() ? "CONNECTED" : "UNPLUGGED");
  Serial.println("--------------------------------------------------\n");

  // --- Audio Init ---
  audioLogger = &Serial; 
  file = new AudioFileSourceFS(SD_MMC); // <--- Bound to Native SD_MMC!
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT); // New EXT1 Header Pins
  out->SetGain(0.2);
  mp3 = new AudioGeneratorMP3();
     
 server = new WiFiServer(currentPort); 
  
 
  server->begin();

  // ---> THE RECOVERY FIX: Check flash memory for previous state <---
  preferences.begin("audio", true); // 'true' means open in Read-Only mode
  bool resumePlay = preferences.getBool("auto_play", false); // Defaults to false
  preferences.end();

  if (resumePlay) {
    Serial.println("[System] Power restored! Resuming previous PLAY state...");
    reqPlay = true; // This will trigger Core 1 to start the music instantly!
  }

  // --- Boot Core 0 Task ---
  Serial.print("[System] Booting Core 0 Network Task... ");
  xTaskCreatePinnedToCore(networkTaskCode, "NetTask", 10000, NULL, 1, &NetworkTaskHandle, 0);
  Serial.println("Done.");
}

// void loop() {
//   // 1. Process flags requested by Core 0
//   if (reqStop) {
//     stopAudio();
//     reqStop = false;
//     Serial.println("[Core 1] Audio Stopped.");
//   }


//   if (reqPlay) {
//     stopAudio();
//     if (file->open("/song.mp3")) {
//       // ---> THE FIX: Shrink to 4KB so we don't run out of memory! <---
//       buff = new AudioFileSourceBuffer(file, 4096); 
//       delay(50); 
//       mp3->begin(buff, out);
//       isAudioPlaying = true;
//       Serial.println("[Core 1] Audio Playing (Buffered Native 4KB).");
//     } else {
//       Serial.println("[Core 1] ERROR: Could not open song.mp3");
//     }
//     reqPlay = false;
//   }

//  // 2. Feed the Audio DMA
//   if (mp3->isRunning()) {
//     if (!mp3->loop()) {
//       // Song ended normally
//       stopAudio(); 
//       if (file->open("/song.mp3")) {
//         // ---> THE FIX: Shrink to 4KB here too! <---
//         buff = new AudioFileSourceBuffer(file, 4096);
//         delay(50); 
//         mp3->begin(buff, out);
//         isAudioPlaying = true;
//       }
//     }
//   } else {
//     // If not playing, yield a tiny bit of CPU so we don't cook the chip
//     delay(1);
//   }
// }


void loop() {
  // 1. Process flags requested by Core 0
  if (reqStop) {
    stopAudio();
    reqStop = false;
    Serial.println("[Core 1] Audio Stopped.");
  }

  if (reqPlay) {
    stopAudio();
    vTaskDelay(100 / portTICK_PERIOD_MS); // <--- ADD THIS
    if (file->open("/song.mp3")) {
      
      // THE FIX: Feed the ID3 reader directly into the MP3 decoder!
      // No more memory-hogging RAM buffers!
      id3 = new AudioFileSourceID3(file);
      vTaskDelay(50 / portTICK_PERIOD_MS); // <--- ADD THIS
      mp3->begin(id3, out); 
      
      isAudioPlaying = true;
      Serial.println("[Core 1] Audio Playing (Direct SD_MMC Stream).");
    } else {
      Serial.println("[Core 1] ERROR: Could not open song.mp3");
    }
    reqPlay = false;
  }

  // 2. Feed the Audio DMA
  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      // Song ended normally
      stopAudio(); 
      if (file->open("/song.mp3")) {
        id3 = new AudioFileSourceID3(file);
        mp3->begin(id3, out); // Direct stream here too
        isAudioPlaying = true;
      }
    }
  } else {
    delay(1);
  }
}

