#include <ESP8266WiFi.h>
#include <DHT.h>
#include <Servo.h>
#include <Firebase_ESP_Client.h>

// Menyertakan token dan helper RTDB dari library Firebase
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ================= PENGATURAN JARINGAN =================
const char* ssid = "LABKESDA 2";
const char* password = "hematologi";

// ================= PENGATURAN FIREBASE =================
#define FIREBASE_URL "smart-home-jemuran-pintar-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_SECRET "fG6By26vxIo7Nf5Knm7ucDMy46ecjrfHD79rINJn"

// ================= PENGATURAN PIN =================
#define DHTPIN D5
#define DHTTYPE DHT11
#define RAIN_PIN D3
#define SERVO_PIN D4

DHT dht(DHTPIN, DHTTYPE);
Servo servo;

// ================= OBJEK FIREBASE =================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ================= VARIABEL SISTEM =================
unsigned long sendDataPrevMillis = 0;
const long interval = 2000; // Interval pengiriman data: 2 detik

void setup() {
  Serial.begin(115200);
  
  // 1. Inisialisasi Sensor & Servo
  dht.begin();
  pinMode(RAIN_PIN, INPUT);
  servo.attach(SERVO_PIN);
  servo.write(0); // Posisi default: keluar/terbuka

  // 2. Koneksi WiFi
  Serial.println("\n--- Memulai Sistem Jemuran Pintar ---");
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan ke WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Berhasil Terhubung!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // 3. Konfigurasi Firebase (Tanpa Autentikasi/Database Secret)
  config.database_url = FIREBASE_URL;
  config.signer.tokens.legacy_token = FIREBASE_SECRET;

  // Ukuran buffer SSL untuk ESP8266 DINAIKKAN agar tidak gagal koneksi (mConnectBasicClient)
  fbdo.setBSSLBufferSize(2048, 1024);

  // Menambahkan waktu tunggu (timeout) agar lebih stabil terhadap WiFi yang lambat
  config.timeout.socketConnection = 10 * 1000; // Tunggu 10 detik sebelum error
  config.timeout.wifiReconnect = 10 * 1000;

  // Memulai koneksi Firebase
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  Serial.println("Sistem Siap! Menunggu data loop...\n");
}

void loop() {
  // Mengeksekusi loop setiap 'interval' (2 detik) TANPA delay() yang memblokir proses
  if (Firebase.ready() && (millis() - sendDataPrevMillis > interval || sendDataPrevMillis == 0)) {
    sendDataPrevMillis = millis();

    Serial.println("==== SIKLUS PEMBACAAN BARU ====");

    // ---------------------------------------------------------
    // 1. BACA SENSOR & ERROR HANDLING
    // ---------------------------------------------------------
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();
    bool isRain = (digitalRead(RAIN_PIN) == LOW); // LOW biasanya berarti basah/hujan di module ini

    // Cek jika pembacaan gagal
    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("[ERROR] Gagal membaca sensor DHT11! Periksa kabel.");
      // Beri nilai default sementara agar logika tidak kacau
      humidity = 0.0;
      temperature = 0.0; 
    }

    Serial.print("Suhu: "); Serial.print(temperature); Serial.println(" C");
    Serial.print("Kelembaban: "); Serial.print(humidity); Serial.println(" %");
    Serial.print("Hujan: "); Serial.println(isRain ? "YA" : "TIDAK");

    // ---------------------------------------------------------
    // 2. KIRIM DATA SENSOR KE FIREBASE
    // ---------------------------------------------------------
    FirebaseJson jsonSensor;
    jsonSensor.set("suhu", temperature);
    jsonSensor.set("kelembaban", humidity);
    jsonSensor.set("hujan", isRain);
    
    // Kirim objek JSON ke path /jemuran_iot/sensor
    if (Firebase.RTDB.setJSON(&fbdo, "/jemuran_iot/sensor", &jsonSensor)) {
      Serial.println("[FIREBASE] Data sensor berhasil dikirim!");
    } else {
      Serial.println("[ERROR FIREBASE] Gagal kirim sensor: " + fbdo.errorReason());
    }

    // ---------------------------------------------------------
    // 3. AMBIL DATA PENGATURAN (KONTROL) DARI FIREBASE
    // ---------------------------------------------------------
    String mode = "otomatis";
    String posisi_plang = "keluar";
    bool smart_climate = false;
    bool hanya_peringatan = false;

    // Ambil variabel dengan ERROR HANDLING & DATA CLEANING
    if(Firebase.RTDB.getString(&fbdo, "/jemuran_iot/kontrol/mode")) {
      mode = fbdo.stringData();
      mode.trim(); // Menghapus spasi liar (contoh: " otomatis " jadi "otomatis")
      mode.toLowerCase(); // Mengubah huruf besar ke kecil (contoh: "Otomatis" jadi "otomatis")
    }
    
    if(Firebase.RTDB.getString(&fbdo, "/jemuran_iot/kontrol/posisi_plang")) {
      posisi_plang = fbdo.stringData();
      posisi_plang.trim();
      posisi_plang.toLowerCase();
    }
    
    if(Firebase.RTDB.getBool(&fbdo, "/jemuran_iot/kontrol/smart_climate")) {
      smart_climate = fbdo.boolData();
    }
    
    if(Firebase.RTDB.getBool(&fbdo, "/jemuran_iot/kontrol/hanya_peringatan")) {
      hanya_peringatan = fbdo.boolData();
    }

    Serial.printf("[DB] Mode: %s | Posisi: %s | Smart: %d | Warning Only: %d\n", 
                  mode.c_str(), posisi_plang.c_str(), smart_climate, hanya_peringatan);

    // ---------------------------------------------------------
    // 4. LOGIKA UTAMA (CORE SYSTEM)
    // ---------------------------------------------------------
    String next_posisi = posisi_plang; // Defaultnya adalah mempertahankan posisi saat ini

    if (mode == "manual") {
      // MODE MANUAL: Ikuti perintah dari Firebase sepenuhnya (dari tombol Flutter)
      next_posisi = posisi_plang;
      Serial.println(">> MODE MANUAL AKTIF");
      
    } else if (mode == "otomatis") {
      Serial.println(">> MODE OTOMATIS AKTIF");
      
      if (hanya_peringatan) {
        // FITUR EARLY WARNING ONLY: Servo dimatikan, hanya jadi stasiun cuaca
        Serial.println(">> FITUR HANYA PERINGATAN AKTIF: Servo tidak akan bergerak.");
        next_posisi = posisi_plang; // Biarkan seperti posisi semula
        
      } else {
        // LOGIKA PENJEMURAN OTOMATIS
        if (isRain) {
          next_posisi = "masuk";
          Serial.println(">> AKSI: HUJAN TERDETEKSI -> JEMURAN MASUK");
          
        } else if (smart_climate && humidity >= 85.0) { // Diubah menjadi >= 85 agar 85.00 pas bisa terbaca
          next_posisi = "masuk";
          Serial.println(">> AKSI: SMART CLIMATE (SANGAT LEMBAB) -> JEMURAN MASUK");
          
        } else {
          next_posisi = "keluar";
          Serial.println(">> AKSI: CUACA AMAN -> JEMURAN KELUAR");
        }
      }
    } else {
      Serial.println("[WARNING] Mode tidak dikenali! Pastikan penulisan di Firebase adalah 'otomatis' atau 'manual'.");
    }

    // ---------------------------------------------------------
    // 5. EKSEKUSI MOTOR SERVO & SINKRONISASI DATABASE
    // ---------------------------------------------------------
    if (next_posisi == "masuk") {
      servo.write(180); 
    } else {
      servo.write(0);
    }

    // Jika sistem otomatis yang menggerakkan servo (sehingga berbeda dengan di Firebase), 
    // update status di Firebase agar UI di Flutter ikut berubah secara realtime!
    if (next_posisi != posisi_plang) {
      if (Firebase.RTDB.setString(&fbdo, "/jemuran_iot/kontrol/posisi_plang", next_posisi)) {
        Serial.println("[SINKRONISASI] Posisi baru (" + next_posisi + ") di-update ke Firebase!");
      }
    }

    Serial.println("===============================\n");
  }
}