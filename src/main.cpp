#include <M5Unified.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <LittleFS.h>
#define SPIFFS LittleFS

// BLE関連のグローバル変数
BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
bool isConnected = false;
uint32_t connectionCount = 0;
bool lastState = false;

// RSA関連のグローバル変数
mbedtls_pk_context pk;
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;
bool rsaInitialized = false;

// メッセージ履歴
String lastMessage = "";
int messageCount = 0;
bool messageDisplayed = false;  // メッセージが表示されているか

// 時間計測用
unsigned long startTime = 0;
unsigned long lastUpdate = 0;
unsigned long lastGC = 0;

// BLEサーバーコールバッククラス
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    isConnected = true;
    connectionCount++;
    Serial.println("========================================");
    Serial.printf("✓ Device connected (total: %d)\n", connectionCount);
    Serial.println("========================================");
  }

  void onDisconnect(BLEServer* pServer) {
    isConnected = false;
    Serial.println("========================================");
    Serial.println("✗ Device disconnected");
    Serial.println("========================================");
    connectionCount--;
    // 切断されたら再度アドバタイズ開始
    BLEDevice::startAdvertising();
    Serial.println("Restarted advertising");
  }
};

// 画面を指定色でクリア
void fillScreen(uint32_t color) {
  M5.Display.fillScreen(color);
}

// ラベル表示用関数
void drawLabel(const char* text, int x, int y, int textSize, uint32_t fgColor, uint32_t bgColor) {
  M5.Display.setTextColor(fgColor, bgColor);
  M5.Display.setTextSize(textSize);
  M5.Display.setCursor(x, y);
  M5.Display.print(text);
}

// ステータス表示を更新
void updateStatus(const char* status) {
  drawLabel(status, 10, 20, 2, TFT_WHITE, TFT_BLACK);
  Serial.println(status);
}

// 情報表示を更新
void updateInfo(const char* info) {
  drawLabel(info, 10, 60, 2, TFT_GREEN, TFT_BLACK);
}

// データ表示を更新
void updateData(const char* data) {
  drawLabel(data, 10, 100, 2, TFT_YELLOW, TFT_BLACK);
}

// メッセージ履歴を表示
void displayMessage(const char* message) {
  // 画面をクリアして再描画
  fillScreen(TFT_GREEN);
  
  // ヘッダー
  drawLabel("Message Received!", 10, 10, 2, TFT_WHITE, TFT_GREEN);
  
  // メッセージカウント
  char countBuf[32];
  snprintf(countBuf, sizeof(countBuf), "Count: %d", messageCount);
  drawLabel(countBuf, 10, 40, 2, TFT_YELLOW, TFT_GREEN);
  
  // 受信メッセージ（複数行対応）
  M5.Display.setTextColor(TFT_BLACK, TFT_GREEN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 80);
  M5.Display.println("Message:");
  
  // メッセージを表示（長い場合は折り返し）
  M5.Display.setCursor(10, 110);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(TFT_WHITE, TFT_GREEN);
  M5.Display.println(message);
  
  lastMessage = String(message);
  messageDisplayed = true;  // メッセージ表示フラグをON
}

// RSA秘密鍵の初期化
bool initRSA() {
  mbedtls_pk_init(&pk);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  
  const char *pers = "rsa_decrypt";
  int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char *)pers, strlen(pers));
  if (ret != 0) {
    Serial.printf("Failed to seed random number generator: -0x%04x\n", -ret);
    return false;
  }
  
  // SPIFFSから秘密鍵を読み込む
  File keyFile = SPIFFS.open("/private.pem", "r");
  if (!keyFile) {
    Serial.println("Failed to open private.pem");
    return false;
  }
  
  size_t keySize = keyFile.size();
  char* keyBuffer = (char*)malloc(keySize + 1);
  if (!keyBuffer) {
    Serial.println("Failed to allocate memory for key");
    keyFile.close();
    return false;
  }
  
  keyFile.readBytes(keyBuffer, keySize);
  keyBuffer[keySize] = '\0';
  keyFile.close();
  
  // 秘密鍵をパース
  ret = mbedtls_pk_parse_key(&pk, (unsigned char*)keyBuffer, keySize + 1, NULL, 0);
  free(keyBuffer);
  
  if (ret != 0) {
    Serial.printf("Failed to parse private key: -0x%04x\n", -ret);
    return false;
  }
  
  Serial.println("RSA private key loaded successfully");
  return true;
}

// RSAで復号化
bool decryptRSA(const uint8_t* encryptedData, size_t encryptedLen, uint8_t* decryptedData, size_t* decryptedLen) {
  if (!rsaInitialized) {
    Serial.println("RSA not initialized");
    return false;
  }
  
  int ret = mbedtls_pk_decrypt(&pk, encryptedData, encryptedLen,
                                decryptedData, decryptedLen, 256,
                                mbedtls_ctr_drbg_random, &ctr_drbg);
  
  if (ret != 0) {
    char error_buf[100];
    mbedtls_strerror(ret, error_buf, sizeof(error_buf));
    Serial.printf("Decryption failed: -0x%04x (%s)\n", -ret, error_buf);
    return false;
  }
  
  return true;
}

// BLE Characteristicコールバッククラス
class MyCharacteristicCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    Serial.println("\n========================================");
    Serial.println("📩 DATA RECEIVED!");
    Serial.println("========================================");
    
    std::string value = pCharacteristic->getValue();
    
    if (value.length() > 0) {
      Serial.printf("Length: %d bytes\n", value.length());
      
      // 16進数で表示
      Serial.print("Hex: ");
      for (int i = 0; i < value.length(); i++) {
        Serial.printf("%02X ", (uint8_t)value[i]);
      }
      Serial.println();
      
      // 文字列として表示
      Serial.print("Text: ");
      Serial.println(value.c_str());
      Serial.println("========================================\n");
      
      // 受信したデータをそのまま表示
      messageCount++;
      displayMessage(value.c_str());
      
      pCharacteristic->setValue("Received!");
      pCharacteristic->notify();
      
      Serial.println("✓ Displayed on screen and sent response");
    } else {
      Serial.println("⚠️  Empty data received");
    }
  }
};

void setup() {
  // M5Stackの初期化
  M5.begin();
  M5.Display.setRotation(1);
  fillScreen(TFT_BLACK);
  
  updateStatus("Starting...");
  Serial.println("=== M5Stack BLE RSA Encryption ===");
  delay(1000);
  
  // SPIFFS初期化（オプション）
  updateStatus("Init FS...");
  Serial.println("Mounting LittleFS...");
  if (!SPIFFS.begin(true)) {
    updateStatus("FS Failed");
    Serial.println("ERROR: Failed to mount LittleFS - RSA will be disabled");
    rsaInitialized = false;
    // SPIFFSが失敗してもBLEは起動する
  } else {
    Serial.println("SUCCESS: LittleFS mounted");
    
    // ファイルリストを表示
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    Serial.println("Files in filesystem:");
    while(file){
      Serial.printf("  - %s (%d bytes)\n", file.name(), file.size());
      file = root.openNextFile();
    }
    
    // RSA初期化（オプション）
    updateStatus("Init RSA...");
    Serial.println("Initializing RSA...");
    rsaInitialized = initRSA();
    if (!rsaInitialized) {
      updateStatus("RSA Failed");
      Serial.println("ERROR: Failed to initialize RSA - continuing without encryption");
      // RSAが失敗してもBLEは起動する
    } else {
      Serial.println("SUCCESS: RSA initialized");
    }
  }
  
  try {
    // BLE初期化ステップ
    updateStatus("Init BLE...");
    delay(1000);
    
    // BLEデバイスの初期化
    BLEDevice::init("M5Stack-BLE");
    delay(1000);
    
    updateStatus("Activating...");
    delay(1000);
    
    // BLEサーバーの作成
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    delay(1000);
    
    updateStatus("Set IRQ...");
    delay(1000);
    
    // BLEサービスの作成(UUIDは例)
    BLEService *pService = pServer->createService("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
    
    // BLE Characteristicの作成(データ送受信用、Notifyサポート)
    pCharacteristic = pService->createCharacteristic(
                                           "beb5483e-36e1-4688-b7f5-ea07361b26a8",
                                           BLECharacteristic::PROPERTY_READ |
                                           BLECharacteristic::PROPERTY_WRITE |
                                           BLECharacteristic::PROPERTY_NOTIFY
                                         );
    pCharacteristic->addDescriptor(new BLE2902());
    pCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
    
    if (rsaInitialized) {
      pCharacteristic->setValue("Ready for encrypted data");
    } else {
      pCharacteristic->setValue("RSA not available - plain text only");
    }
    pService->start();
    
    updateStatus("Advertising...");
    delay(1000);
    
    // アドバタイズの開始
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // iPhone接続の問題を解決するのに役立つ
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    
    updateStatus("BLE Ready!");
    if (rsaInitialized) {
      updateInfo("RSA: Enabled");
    } else {
      updateInfo("RSA: Disabled");
    }
    fillScreen(TFT_BLUE);
    updateStatus("BLE Ready!");
    if (rsaInitialized) {
      updateInfo("RSA: Enabled");
    } else {
      updateInfo("RSA: Disabled");
    }
    
    Serial.println("BLE Ready - waiting for connection");
    if (rsaInitialized) {
      Serial.println("RSA encryption is enabled");
    } else {
      Serial.println("RSA encryption is disabled");
    };
    
    startTime = millis();
    lastUpdate = millis();
    lastGC = millis();
    
  } catch (...) {
    updateStatus("ERROR");
    updateInfo("Init Failed");
    fillScreen(TFT_RED);
    Serial.println("Error: BLE initialization failed");
  }
}

void loop() {
  M5.update();
  
  unsigned long currentTime = millis();
  
  // 接続状態が変化したら画面更新
  if (isConnected != lastState) {
    messageDisplayed = false;  // 接続状態変化時はメッセージ表示をリセット
    
    if (isConnected) {
      fillScreen(TFT_GREEN);  // 緑 = 接続
      drawLabel("Connected!", 10, 20, 3, TFT_WHITE, TFT_GREEN);
      drawLabel("Waiting for", 10, 80, 2, TFT_BLACK, TFT_GREEN);
      drawLabel("encrypted data...", 10, 110, 2, TFT_BLACK, TFT_GREEN);
      
      if (rsaInitialized) {
        drawLabel("RSA: Ready", 10, 160, 2, TFT_YELLOW, TFT_GREEN);
      } else {
        drawLabel("RSA: Disabled", 10, 160, 2, TFT_RED, TFT_GREEN);
      }
    } else {
      fillScreen(TFT_BLUE);  // 青 = 待機
      updateStatus("Waiting...");
      if (rsaInitialized) {
        updateInfo("RSA: Enabled");
      } else {
        updateInfo("RSA: Disabled");
      }
      
      // メッセージカウントを表示
      if (messageCount > 0) {
        char buf[32];
        sprintf(buf, "Messages: %d", messageCount);
        updateData(buf);
      }
    }
    lastState = isConnected;
  }
  
  // 5秒ごとに経過時間を画面表示（メッセージ表示中は表示しない）
  if (!messageDisplayed && currentTime - lastUpdate >= 5000) {
    // 接続待機中のみ時間を表示
    if (!isConnected) {
      char buf[32];
      sprintf(buf, "Time: %lus", (currentTime - startTime) / 1000);
      updateData(buf);
    }
    lastUpdate = currentTime;
  }
  
  // 10秒ごとにメモリ情報をシリアル出力
  if (currentTime - lastGC >= 10000) {
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    lastGC = currentTime;
  }
  
  delay(200);
}