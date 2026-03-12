# WiFi-Manager

> Manajemen jaringan WiFi dengan NTP, RTC backup, dan antarmuka web — berbasis ESP32

[![Version](https://img.shields.io/badge/version-3.0-blue)](https://github.com/gonit-dev/jws-indonesia) [![Platform](https://img.shields.io/badge/platform-ESP32-red)](https://www.espressif.com/)

---

## ✨ Fitur Utama

### 🌐 Jaringan
- **Dual WiFi Mode** — AP + STA bersamaan (akses via router maupun AP lokal)
- **Smart AP Selection** — Scan semua channel, pilih BSSID dengan RSSI terkuat otomatis. Berguna untuk jaringan multi-AP (WISP, WDS, mesh)
- **Scan Hanya Saat Diperlukan** — Hanya saat boot, koneksi terputus, atau retry. Tidak scan saat koneksi aktif
- **Auto-Reconnect** — Event-driven tanpa polling, hingga 5 percobaan
- **WiFi Failed + Exponential Backoff** — Setelah 5x gagal, retry otomatis dengan jeda makin panjang (10s → 20s → 40s → 60s → 120s maks), retry selamanya sampai berhasil
- **Custom AP** — Konfigurasi SSID, Password, IP, Gateway, Subnet
- **WiFi Sleep Disabled** — Performa maksimal, response time cepat
- **Connection Type Detection** — Deteksi otomatis apakah client akses via AP atau router
- **Internet Check** — Cek koneksi internet setiap 30 detik via TCP ke `8.8.8.8:53`

### ⏰ Manajemen Waktu
- **NTP Auto-Sync** setiap 1 jam dengan 3 fallback server (`pool.ntp.org`, `time.google.com`, `time.windows.com`)
- **Zona Waktu** — Dukungan UTC-12 hingga UTC+14 (WIB/WITA/WIT)
- **RTC Backup** — DS3231 opsional untuk persistensi waktu saat mati lampu
- **Manual Sync** — Sync waktu dari browser jika diperlukan
- **Clock Tick** — Increment waktu tiap detik via FreeRTOS task

### 🔴 RGB LED Status Indicator
| Kondisi | Warna | Mode |
|---------|-------|------|
| Booting | Merah | Kedip cepat (300ms) |
| Boot selesai, WiFi tidak dikonfigurasi | — | Mati |
| Sedang connecting ke router | Hijau | Kedip cepat (300ms) |
| Terhubung ke router + internet OK | Hijau | Nyala solid |
| Terhubung ke router + internet putus | Hijau | Kedip lambat (500ms) |
| Gagal konek ke router (WIFI_FAILED) | Merah | Nyala |
| Countdown restart/factory reset | Merah | Kedip (500ms) |

> Cek internet dilakukan setiap 30 detik via TCP connect ke `8.8.8.8:53` (DNS Google) — ringan, tidak ada data yang dikirim.

### 🖥️ Web Interface
- **Responsive design** — mobile-friendly
- **Countdown Safety** — Progress bar visual untuk restart/reset (60 detik)
- **Real-time display** — Jam, tanggal, status WiFi, uptime

### 💾 Penyimpanan
- **LittleFS** — Semua konfigurasi persistent
- **Auto-Create Default** — File konfigurasi dibuat otomatis saat boot pertama
- **Auto-Save** — Simpan otomatis setelah setiap perubahan
- **Factory Reset** — Kembalikan ke default dengan safety countdown

### 🛡️ Stabilitas Sistem
- **Stack Monitoring** — Laporan penggunaan stack setiap 2 menit
- **Memory Monitoring** — Laporan heap setiap 30 detik
- **Hardware Watchdog** — ESP32 WDT dengan timeout 100 detik
- **AP Health Check** — Periksa dan restart AP mode otomatis setiap 5 detik jika mode berubah

---

## 🔧 Hardware

### Board: ESP32 (diuji pada ESP32-2432S024)
- **MCU:** ESP32 Dual-Core @ 240MHz
- **RAM:** 520KB SRAM
- **Flash:** 4MB (minimal)
- **WiFi:** 802.11 b/g/n (2.4GHz)
- **Power:** 5V USB (minimal 1A)

### Komponen Opsional

#### RGB LED (Indikator Status)
```
LED RGB Common Anode
Anoda (+) → 3.3V (melalui resistor)
R (Merah) → GPIO4
G (Hijau) → GPIO16
B (Biru)  → GPIO17
```

#### RTC DS3231 (Sangat Disarankan)
```
DS3231 VCC → ESP32 3.3V
DS3231 GND → ESP32 GND
DS3231 SDA → ESP32 GPIO21
DS3231 SCL → ESP32 GPIO22
Baterai CR2032 → Slot baterai RTC
```

**Manfaat:**
- Backup waktu saat mati lampu
- Akurasi ±2ppm dengan temperature compensation
- Auto-sync dari RTC ke sistem setiap 1 menit jika NTP belum tersedia

**⚠️ Tanpa RTC:** Waktu reset ke 01/01/2000 setiap restart hingga NTP sync berhasil.

---

## 📦 Instalasi

### 1. Kebutuhan Library

| Library | Versi | Catatan |
|---------|-------|---------|
| ESP32 Board | v3.0.7 | ESP32 Core for Arduino |
| ESPAsyncWebServer | 3.x | Untuk ESP32 Core v3.x |
| AsyncTCP | Latest | Dependency ESPAsync |
| TimeLib | 1.6.1+ | |
| RTClib | 2.1.1+ | Untuk DS3231 |

**⚠️ PENTING:** Gunakan ESP32 Core v3.0.7, bukan v2.x — ada breaking changes di WiFi API.

### 2. Install ESP32 Board

```
File → Preferences → Additional Boards Manager URLs:
https://espressif.github.io/arduino-esp32/package_esp32_index.json

Tools → Board → Boards Manager → Cari "esp32"
Install: esp32 by Espressif Systems v3.0.7
```

### 3. Upload Filesystem (LittleFS)

**Install Plugin:**
1. Download: https://github.com/lorol/arduino-esp32littlefs-plugin/releases
2. Extract ke `Arduino/tools/`
3. Restart Arduino IDE

**Upload Data:**
1. Buat folder `data/` di root sketch
2. Copy:
   - `index.html`
   - `css/foundation.min.css`
3. Tools → ESP32 Sketch Data Upload

**⚠️ Upload filesystem dulu sebelum upload sketch!**

### 4. Upload Sketch

```
Board: ESP32 Dev Module
Upload Speed: 921600
CPU Frequency: 240MHz
Flash Frequency: 80MHz
Flash Mode: QIO
Flash Size: 4MB (2MB APP / 2MB SPIFFS)
Partition Scheme: Default 4MB with spiffs
```

---

## 🚀 Panduan Cepat

### Boot Pertama
```
AP SSID:     JWS-(id unik)
AP Password: 12345678
AP IP:       http://192.168.100.1
```

### Setup WiFi
1. Sambungkan ke AP `JWS-(id unik)`
2. Buka browser → `http://192.168.100.1`
3. Tab **WIFI** → Input SSID dan Password WiFi
4. Klik **Simpan** → Tunggu ~30 detik

### Setup Zona Waktu
```
Default: UTC+7 (WIB)

Indonesia:
- WIB:  +7  (Jawa, Sumatera, Kalimantan Barat)
- WITA: +8  (Kalimantan Tengah/Timur, Sulawesi, Bali)
- WIT:  +9  (Papua, Maluku)
```
Tab **WAKTU** → Klik ikon 🕐 → Input offset → Klik 💾

---

## 🌐 Web Interface

### Tab BERANDA
- Status koneksi WiFi dan IP address
- Badge status realtime: Terhubung / Menghubungkan / Gagal / Tidak terkonfigurasi
- Kekuatan sinyal (dBm) + label kualitas
- Status NTP dan server aktif
- Jam dan tanggal real-time
- Uptime perangkat
- Tombol restart

### Tab WIFI
- **WiFi Router:** SSID dan Password
- **Access Point:** Custom SSID, Password, IP, Gateway, Subnet
- Validasi format otomatis
- Restart dengan countdown 60 detik

### Tab WAKTU
- **Manual Sync:** Ambil waktu dari browser
- **Auto NTP:** Sync setiap 1 jam, 3 fallback server
- **Zona Waktu:** Inline edit UTC-12 hingga UTC+14
- Auto-trigger NTP sync saat zona waktu diubah

### Tab RESET
- **Factory Reset:** Hapus semua konfigurasi, countdown 60 detik
- Auto-redirect ke `192.168.100.1`

---

## ⚙️ Konfigurasi Default

File dibuat otomatis saat boot pertama. File yang sudah ada **tidak ditimpa**.

| File | Isi Default |
|------|-------------|
| `/ap_creds.txt` | SSID: `JWS-<MAC>`, Password: `12345678`, IP: `192.168.100.1` |
| `/timezone.txt` | `7` (UTC+7 / WIB) |

File berikut **tidak dibuat otomatis**:

| File | Alasan |
|------|--------|
| `/wifi_creds.txt` | Diisi user via web interface |

---

## 📊 API Endpoints

### GET

| Endpoint | Deskripsi |
|----------|-----------|
| `/` | Web interface utama |
| `/devicestatus` | Status lengkap perangkat (WiFi, NTP, waktu, heap) |
| `/getwificonfig` | Konfigurasi WiFi router & AP |
| `/gettimezone` | Offset timezone aktif |
| `/api/data` | Data real-time (JSON, cocok untuk IoT/Home Assistant) |
| `/api/countdown` | Status countdown restart/reset |
| `/api/connection-type` | Tipe koneksi client (via AP atau router) |

### POST

| Endpoint | Parameter | Deskripsi |
|----------|-----------|-----------|
| `/restart` | — | Restart perangkat (countdown 60s) |
| `/reset` | — | Factory reset (countdown 60s) |
| `/setwifi` | `ssid`, `password` | Set kredensial router |
| `/setap` | `ssid`, `password` | Set SSID dan password AP |
| `/setap` | `updateNetworkConfig=true`, `apIP`, `gateway`, `subnet` | Set IP/Gateway/Subnet AP |
| `/synctime` | `y`,`m`,`d`,`h`,`i`,`s` | Sync waktu manual dari browser |
| `/settimezone` | `offset` | Set UTC offset |

### Contoh Response `/api/data`
```json
{
  "time": "14:35:22",
  "date": "19/12/2024",
  "day": "Wednesday",
  "timestamp": 1734614122,
  "device": {
    "wifiConnected": true,
    "wifiState": "connected",
    "rssi": -52,
    "apIP": "192.168.100.1",
    "ntpSynced": true,
    "ntpServer": "pool.ntp.org",
    "freeHeap": 245632,
    "uptime": 3600
  }
}
```

Nilai `wifiState`:

| Value | Keterangan |
|-------|-----------|
| `idle` | WiFi belum dikonfigurasi |
| `connecting` | Sedang menghubungkan ke router |
| `connected` | Terhubung ke router |
| `failed` | Gagal konek — sedang exponential backoff retry |

---

## 🌐 WiFi — Detail Mekanisme

### Smart AP Selection (Multi-AP / WISP / WDS)

Saat konek atau reconnect, sistem scan semua channel terlebih dahulu lalu memilih BSSID dengan RSSI terkuat.

```
[WIFI] MEMINDAI AP TERKUAT (ASYNC)...
  [0] BSSID: AA:BB:CC:DD:EE:01 | RSSI: -72 dBm | CH: 6
  [1] BSSID: AA:BB:CC:DD:EE:02 | RSSI: -45 dBm | CH: 11
[WIFI] AP TERPILIH: AA:BB:CC:DD:EE:02 | RSSI: -45 dBm
```

Scan hanya terjadi saat: boot, WiFi terputus, retry setelah WIFI_FAILED, atau restart WiFi manual. **Tidak scan saat koneksi aktif.**

### Exponential Backoff

| Retry ke- | Jeda |
|-----------|------|
| 1 | 10 detik |
| 2 | 20 detik |
| 3 | 40 detik |
| 4 | 60 detik |
| 5+ | 120 detik (retry selamanya) |

---

## 🔍 Troubleshooting

### WiFi Tidak Connect / WIFI_FAILED

**Penyebab umum:**
- SSID/Password salah (case-sensitive)
- Router hanya 5GHz (ESP32 hanya 2.4GHz)
- Sinyal terlalu lemah
- Channel WiFi > 11

**Indikator LED:**
```
Hijau kedip cepat  → Sedang connecting
Hijau solid        → Konek + internet OK
Hijau kedip lambat → Konek, internet putus
Merah nyala        → WIFI_FAILED — retry otomatis
LED mati           → WiFi tidak dikonfigurasi
```

**RSSI Guide:**
- `-50 dBm` → Sangat baik
- `-60 dBm` → Baik
- `-70 dBm` → Cukup
- `-80 dBm` → Lemah

### Jam Reset ke 01/01/2000

**Penyebab:** NTP belum sync, RTC tidak terpasang, atau baterai RTC habis.

**Solusi:**
1. Sambungkan WiFi — NTP sync otomatis dalam ~30 detik
2. Pasang RTC DS3231 + baterai CR2032
3. Tab **WAKTU** → Tombol **Perbarui Waktu** (sementara)

### Internet Putus (WiFi Masih Konek)

**Gejala:** LED hijau kedip lambat.

**Solusi:** Cek kabel LAN/modem. Sistem cek ulang otomatis setiap 30 detik — LED kembali solid saat internet pulih.

### Web Interface Lambat/Timeout

1. Dekatkan ke router (RSSI > -60 dBm)
2. Clear cache browser
3. Tab **BERANDA** → **Mulai Ulang Perangkat**

---

## 🛡️ Keamanan

```
⚠️ GANTI SETELAH BOOT PERTAMA

AP SSID:     JWS-(id unik)
AP Password: 12345678
AP IP:       192.168.100.1
```

**Rekomendasi:**
1. Ganti SSID dan password AP segera setelah setup
2. Gunakan WPA2/WPA3 di router
3. Jangan ekspos ke internet publik — hanya akses lokal

---

## 🙏 Credits

**Developer:** GONIT - Global Network Identification Technology

**Libraries:**
- Espressif Systems — ESP32 Arduino Core
- me-no-dev — ESPAsyncWebServer
- Adafruit — RTClib

---

**© 2025 GONIT - Global Network Identification Technology**
