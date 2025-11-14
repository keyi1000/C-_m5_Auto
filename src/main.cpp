/**
 * Simple M5Stack BLE Peripheral (C++ / Arduino)
 * - 自動接続・再接続処理を実装
 * - 切断時は自動的に広告を再開
 * - 2秒ごとに接続中のクライアントにNotifyを送信
 * 
 * 必要に応じて SERVICE_UUID / CHAR_UUID を iOS 側と合わせてください
 */

#include <M5Unified.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// === UUID設定（実環境に合わせて変更してください） ===
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890AC"
#define CHARACTERISTIC_UUID "87654321-4321-4321-4321-BA0987654321"
#define DEVICE_NAME         "M5-BLE-TEST"
// =====================================================

// グローバル変数
BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;
uint32_t notifyCounter = 0;

// 広告再開フラグ
bool shouldRestartAdvertising = false;

// サーバーコールバック：接続・切断時の処理
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("Central connected");
        
        // M5Unified画面表示（オプション）
        M5.Display.fillRect(0, 40, 320, 20, BLACK);
        M5.Display.setCursor(10, 40);
        M5.Display.setTextColor(GREEN);
        M5.Display.println("Status: Connected");
    }

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("Central disconnected");
        
        // 切断時は広告を再開する（メインループで処理）
        shouldRestartAdvertising = true;
        
        // M5Unified画面表示（オプション）
        M5.Display.fillRect(0, 40, 320, 20, BLACK);
        M5.Display.setCursor(10, 40);
        M5.Display.setTextColor(YELLOW);
        M5.Display.println("Status: Disconnected");
    }
};

// キャラクタリスティックコールバック：書き込み時の処理
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        
        if (value.length() > 0) {
            Serial.print("RX: ");
            Serial.println(value.c_str());
            
            // M5Unified画面表示（オプション）
            M5.Display.fillRect(0, 80, 320, 20, BLACK);
            M5.Display.setCursor(10, 80);
            M5.Display.setTextColor(CYAN);
            M5.Display.print("RX: ");
            M5.Display.println(value.c_str());
        }
    }
};

// BLE初期化関数
void initBLE() {
    Serial.println("Initializing BLE...");
    
    // BLEデバイス初期化
    BLEDevice::init(DEVICE_NAME);
    
    // BLEサーバー作成
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    // BLEサービス作成
    BLEService* pService = pServer->createService(SERVICE_UUID);
    
    // BLEキャラクタリスティック作成
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ   |
        BLECharacteristic::PROPERTY_WRITE  |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    
    // ディスクリプタ追加（Notify用）
    pCharacteristic->addDescriptor(new BLE2902());
    pCharacteristic->setCallbacks(new MyCallbacks());
    
    // 初期値設定
    pCharacteristic->setValue("hello");
    
    // サービス開始
    pService->start();
    
    // 広告開始
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // iPhone接続の問題対策
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    
    Serial.println("BLE advertising started");
    Serial.print("Device name: ");
    Serial.println(DEVICE_NAME);
}

// 広告再開関数（切断時に呼ばれる）
void restartAdvertising() {
    delay(500);  // 安定のため少し待つ
    BLEDevice::startAdvertising();
    Serial.println("Advertising restarted");
    
    // M5Unified画面表示（オプション）
    M5.Display.fillRect(0, 60, 320, 20, BLACK);
    M5.Display.setCursor(10, 60);
    M5.Display.setTextColor(MAGENTA);
    M5.Display.println("Advertising restarted");
}

void setup() {
    // M5Unified初期化
    auto cfg = M5.config();
    M5.begin(cfg);
    
    // シリアル初期化
    Serial.begin(115200);
    Serial.println("M5Stack BLE Auto-Connect Example");
    
    // 画面初期化
    M5.Display.setTextSize(2);
    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(10, 10);
    M5.Display.setTextColor(WHITE);
    M5.Display.println("BLE Peripheral");
    
    // BLE初期化
    initBLE();
    
    M5.Display.setCursor(10, 40);
    M5.Display.setTextColor(YELLOW);
    M5.Display.println("Status: Advertising");
}

void loop() {
    M5.update();
    
    // 切断後の広告再開処理
    if (shouldRestartAdvertising) {
        shouldRestartAdvertising = false;
        restartAdvertising();
    }
    
    // 接続状態が変化した時の処理
    if (!deviceConnected && oldDeviceConnected) {
        // 切断された直後（広告再開はshouldRestartAdvertisingで処理）
        oldDeviceConnected = deviceConnected;
    }
    
    // 新規接続された時の処理
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
        Serial.println("New connection established");
    }
    
    // 接続中は2秒ごとにNotifyを送信
    static unsigned long lastNotifyTime = 0;
    if (deviceConnected && (millis() - lastNotifyTime > 2000)) {
        lastNotifyTime = millis();
        
        // Notify送信
        char msg[32];
        snprintf(msg, sizeof(msg), "ping %lu", notifyCounter++);
        pCharacteristic->setValue(msg);
        pCharacteristic->notify();
        
        Serial.print("Notify: ");
        Serial.println(msg);
        
        // M5Unified画面表示（オプション）
        M5.Display.fillRect(0, 100, 320, 20, BLACK);
        M5.Display.setCursor(10, 100);
        M5.Display.setTextColor(GREEN);
        M5.Display.print("TX: ");
        M5.Display.println(msg);
    }
    
    delay(10);  // CPU負荷軽減
}
