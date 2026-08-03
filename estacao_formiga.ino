// ----------------------------------------------
// Estação Meteorológica IoT com ESP8266
// (c) 2025 Jan Caraumã <janderson.gomes@ufrr.br>
// Corrigido em 01 de agosto de 2026
// ----------------------------------------------

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <time.h>
#include <Ticker.h>
#include <LittleFS.h>

// Configurações da rede
const char* ssid = "dlink";
const char* password = "";
const char* apSSID = "Estacao_Formiga";
const char* apPassword = "senha123";

// TODO IP Fixo
//TODO IPAddress apIP(192, 168, 4, 100);
//TODO IPAddress apGateway(192, 168, 4, 1);
//TODO IPAddress apSubnet(255, 255, 255, 0);

// ::: Configuração Gemini (opcional)
// Preencha para habilitar o chat da Ana. Deixe vazio para ocultar o chat.
const char* GEMINI_KEY            = ""; // <-- coloque sua chave/key aqui
const char* GEMINI_MODEL          = "gemini-3.1-flash-lite-preview";
const char* GEMINI_FALLBACK_MODEL = "gemini-1.5-flash";

// ::: Tipos

struct SensorReading {
    float value;
    bool valid;
    String status; // "ok", "nan", "range", "offline"
};

// ::: Histórico persistente (LittleFS)
// Buffer circular gravado na flash interna do ESP8266: sobrevive a
// quedas de energia, reinícios e a uploads normais do sketch (o
// LittleFS fica numa área da flash separada do programa). Só é
// perdido se, ao gravar, você escolher um "Flash Size" no Arduino
// IDE sem reservar espaço pra FS, ou apagar a flash inteira.
//
// Registro compacto de 16 bytes (valores *10 para guardar 1 casa
// decimal em inteiro, sem gastar 4 bytes de float por campo).
#define HIST_FILE       "/historico.bin"
#define HIST_META_FILE  "/hist_meta.bin"
#define HIST_INVALID    (-32768)          // sentinela de "sem leitura válida"
#define HIST_INTERVAL_MS (60UL * 1000UL)  // 1 amostra por minuto

struct __attribute__((packed)) HistRecord {
    uint32_t epoch;      // horário Unix (0 = hora ainda não sincronizada via NTP)
    int16_t  temp_x10;   // temperatura x10 (°C)
    int16_t  umid_x10;   // umidade x10 (%)
    int16_t  press_x10;  // pressão x10 (hPa)
    int16_t  alt_m;      // altitude (m, inteiro)
    uint16_t gas;        // leitura bruta MQ135 (0–1023)
    uint16_t chuva;      // leitura bruta sensor de chuva (0–1023)
};

struct __attribute__((packed)) HistMeta {
    uint32_t capacidade;
    uint32_t writeIndex;
    uint32_t count;
};

bool     histReady      = false;
uint32_t histCapacity   = 0;
uint32_t histWriteIndex = 0;
uint32_t histCount      = 0;
unsigned long lastHistSample = 0;

// ::: Hardware

#define DHTPIN 2
#define DHTTYPE DHT11
#define MQ135_PIN A0
#define RAIN_SENSOR_PIN 14

// Limites de plausibilidade dos sensores
#define TEMP_MIN -10.0
#define TEMP_MAX 60.0
#define UMID_MIN 0.0
#define UMID_MAX 100.0
#define PRESS_MIN 870.0
#define PRESS_MAX 1085.0
#define ALT_MIN -100.0
#define ALT_MAX 5000.0

// Flags de status dos sensores
bool dhtOk = false;
bool bmpOk = false;

Adafruit_BMP085 bmp;

// Objetos globais
ESP8266WebServer server(80);
DHT dht(DHTPIN, DHTTYPE);
Ticker secondTick;

// Variáveis de estado
bool timeValid = false;
unsigned long lastNTPUpdate = 0;
unsigned long localSeconds = 0;
String errorLog = "=== Logs do Sistema ===\n";

// Handlers de eventos WiFi
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;

// ::: Setup

void setup() {
    Serial.begin(115200);
    delay(100);

    dht.begin();
    dhtOk = true;

    bmpOk = bmp.begin();
    if (!bmpOk) {
        logError("BMP180", "Falha na inicialização (I2C não respondeu)");
    } else {
        logInfo("BMP180 inicializado com sucesso");
    }

    histReady = initHistoricoStorage();

    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);

    wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
    wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

    Serial.print("Conectando a ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nFalha na conexão. Iniciando modo AP...");
        logError("WiFi", "Timeout ao conectar em STA. Iniciando modo AP.");
        WiFi.mode(WIFI_AP);
        bool apStatus = WiFi.softAP(apSSID, apPassword);
        Serial.println("AP iniciado: " + String(apStatus ? "OK" : "Falha"));
        Serial.print("IP do AP: ");
        Serial.println(WiFi.softAPIP());
    }

    setupNTP();
    lastNTPUpdate = millis();

    setupMDNS();
    server.on("/", handle_OnConnect);
    server.on("/dados", handle_JSONData);
    server.on("/logs", handle_Logs);
    server.on("/historico", handle_HistData);
    server.on("/historico/limpar", HTTP_POST, handle_HistClear);
    server.onNotFound(handle_NotFound);
    server.begin();
    logInfo("Servidor HTTP iniciado");

    secondTick.attach(1, [](){
        if (millis() % 1000 == 0) ESP.wdtFeed();
    });
    ESP.wdtEnable(10000);

    Serial.print("Heap livre após setup(): ");
    Serial.println(ESP.getFreeHeap());
}

void loop() {
    server.handleClient();
    MDNS.update();

    if (!timeValid && millis() - lastNTPUpdate > 1000) {
        localSeconds++;
        lastNTPUpdate = millis();
    }

    if (histReady && millis() - lastHistSample >= HIST_INTERVAL_MS) {
        lastHistSample = millis();
        registrarHistorico();
    }
}

// ::: WiFi e Tempo

void onWifiConnect(const WiFiEventStationModeGotIP& event) {
    Serial.println("\nConectado ao WiFi!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    logInfo("WiFi reconectado. IP: " + WiFi.localIP().toString());
    setupMDNS();
    setupNTP();
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
    Serial.println("\nDesconectado do WiFi!");
    logError("WiFi", "Conexão perdida (reason: " + String(event.reason) + ")");
    MDNS.close();
}

void setupMDNS() {
    if (WiFi.getMode() == WIFI_AP) return;
    if (!MDNS.begin("estacaoformiga")) {
        logError("mDNS", "Falha ao iniciar");
    } else {
        logInfo("mDNS ativo: estacaoformiga.local");
    }
}

void setupNTP() {
    configTime(-4 * 3600, 0, "pool.ntp.org", "time.nist.gov", "br.pool.ntp.org");
    timeValid = false;
    lastNTPUpdate = millis();
}

String getFormattedTime() {
    time_t now = time(nullptr);
    if (now < 86400 || !timeValid) {
        unsigned long h = localSeconds / 3600;
        unsigned long m = (localSeconds % 3600) / 60;
        unsigned long s = localSeconds % 60;
        char buf[30];
        snprintf(buf, sizeof(buf), "%02luh %02lum %02lus (local)", h, m, s);
        return String(buf);
    }
    struct tm* ti = localtime(&now);
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", ti);
    return String(buffer);
}

// ::: Registros / Logs

void logError(String sensor, String message) {
    String entry = "[ERRO][" + getFormattedTime() + "][" + sensor + "] " + message + "\n";
    errorLog += entry;
    if (errorLog.length() > 4000) errorLog = errorLog.substring(2000);
    Serial.print(entry);
}

void logInfo(String message) {
    String entry = "[INFO][" + getFormattedTime() + "] " + message + "\n";
    errorLog += entry;
    if (errorLog.length() > 4000) errorLog = errorLog.substring(2000);
    Serial.print(entry);
}

// ::: Leitura de Sensores 

SensorReading readDHT(bool isHumidity) {
    SensorReading result = { NAN, false, "nan" };

    if (!dhtOk) {
        result.status = "offline";
        return result;
    }

    for (int i = 0; i < 3; i++) {
        float v = isHumidity ? dht.readHumidity() : dht.readTemperature();
        if (!isnan(v)) {
            float lo = isHumidity ? UMID_MIN : TEMP_MIN;
            float hi = isHumidity ? UMID_MAX : TEMP_MAX;
            if (v < lo || v > hi) {
                result.status = "range";
                logError("DHT11", String(isHumidity ? "Umidade" : "Temperatura") +
                         " fora de range: " + String(v));
                return result;
            }
            result.value = v;
            result.valid = true;
            result.status = "ok";
            return result;
        }
        delay(50);
    }

    logError("DHT11", String(isHumidity ? "Umidade" : "Temperatura") +
             " — 3 leituras consecutivas retornaram NaN");
    dhtOk = false;
    return result;
}

SensorReading readBMPPressure() {
    SensorReading result = { NAN, false, "offline" };
    if (!bmpOk) return result;

    float p = bmp.readPressure() / 100.0;
    if (isnan(p)) {
        logError("BMP180", "Leitura de pressão retornou NaN");
        result.status = "nan";
        return result;
    }
    if (p < PRESS_MIN || p > PRESS_MAX) {
        logError("BMP180", "Pressão fora de range: " + String(p) + " hPa");
        result.status = "range";
        return result;
    }
    result.value = p;
    result.valid = true;
    result.status = "ok";
    return result;
}

SensorReading readBMPAltitude() {
    SensorReading result = { NAN, false, "offline" };
    if (!bmpOk) return result;

    float a = bmp.readAltitude();
    if (isnan(a)) {
        logError("BMP180", "Leitura de altitude retornou NaN");
        result.status = "nan";
        return result;
    }
    if (a < ALT_MIN || a > ALT_MAX) {
        logError("BMP180", "Altitude fora de range: " + String(a) + " m");
        result.status = "range";
        return result;
    }
    result.value = a;
    result.valid = true;
    result.status = "ok";
    return result;
}

String classificarChuva(int valor) {
    if (valor > 600) return "Sem chuva";
    if (valor > 400) return "Chuva leve";
    if (valor > 200) return "Chuva moderada";
    return "Chuva forte";
}

// ::: Histórico persistente (LittleFS)

void salvarHistMeta() {
    File f = LittleFS.open(HIST_META_FILE, "w");
    if (!f) { logError("LittleFS", "Falha ao salvar meta do histórico"); return; }
    HistMeta m = { histCapacity, histWriteIndex, histCount };
    f.write((uint8_t*)&m, sizeof(m));
    f.close();
}

bool initHistoricoStorage() {
    if (!LittleFS.begin()) {
        logError("LittleFS", "Falha ao montar sistema de arquivos (reserve espaço de "
                              "FS em Tools > Flash Size no Arduino IDE)");
        return false;
    }

    FSInfo info;
    LittleFS.info(info);

    // Deixa ~12 KB de folga pra metadados e futuros arquivos, e usa
    // o resto todo pra maximizar quantas amostras cabem.
    const size_t reservado = 12288;
    size_t disponivel = (info.totalBytes > reservado) ? (info.totalBytes - reservado) : 0;
    uint32_t capacidadeCalculada = disponivel / sizeof(HistRecord);
    if (capacidadeCalculada < 10) {
        logError("LittleFS", "Espaço de FS insuficiente para histórico (verifique o "
                              "Flash Size selecionado no Arduino IDE)");
        return false;
    }

    File metaF = LittleFS.open(HIST_META_FILE, "r");
    bool metaValida = false;
    if (metaF) {
        HistMeta m;
        if (metaF.read((uint8_t*)&m, sizeof(m)) == (int)sizeof(m) &&
            m.capacidade == capacidadeCalculada && LittleFS.exists(HIST_FILE)) {
            histCapacity   = m.capacidade;
            histWriteIndex = m.writeIndex;
            histCount      = m.count;
            metaValida     = true;
        }
        metaF.close();
    }

    if (!metaValida) {
        // Primeira vez (ou a capacidade mudou, ex.: reflash com FS maior/menor)
        histCapacity   = capacidadeCalculada;
        histWriteIndex = 0;
        histCount      = 0;
        LittleFS.remove(HIST_FILE);
        File f = LittleFS.open(HIST_FILE, "w");
        if (f) f.close();
        salvarHistMeta();
    }

    logInfo("Histórico: capacidade para " + String(histCapacity) + " amostras (~" +
            String((histCapacity * sizeof(HistRecord)) / 1024) + " KB), " +
            String(histCount) + " já armazenadas");
    return true;
}

// Grava um novo registro no buffer circular: enquanto não estiver
// cheio, só acrescenta no fim (append); quando enche, passa a
// sobrescrever o mais antigo primeiro (roda em círculo).
void histGravarRegistro(const HistRecord& rec) {
    if (!histReady) return;

    if (histCount < histCapacity) {
        File f = LittleFS.open(HIST_FILE, "a");
        if (!f) { logError("LittleFS", "Falha ao abrir histórico para escrita"); return; }
        f.write((uint8_t*)&rec, sizeof(rec));
        f.close();
        histWriteIndex = histCount + 1;
        histCount++;
    } else {
        File f = LittleFS.open(HIST_FILE, "r+");
        if (!f) { logError("LittleFS", "Falha ao abrir histórico para sobrescrita"); return; }
        f.seek((uint32_t)histWriteIndex * sizeof(HistRecord), SeekSet);
        f.write((uint8_t*)&rec, sizeof(rec));
        f.close();
        histWriteIndex = (histWriteIndex + 1) % histCapacity;
    }
    salvarHistMeta();
}

void registrarHistorico() {
    if (!histReady) return;

    SensorReading temp     = readDHT(false);
    SensorReading umid     = readDHT(true);
    SensorReading pressao  = readBMPPressure();
    SensorReading altitude = readBMPAltitude();
    int chuva = analogRead(RAIN_SENSOR_PIN);
    int gas   = analogRead(MQ135_PIN);

    HistRecord rec;
    rec.epoch     = timeValid ? (uint32_t)time(nullptr) : 0;
    rec.temp_x10  = temp.valid     ? (int16_t)round(temp.value * 10.0)     : HIST_INVALID;
    rec.umid_x10  = umid.valid     ? (int16_t)round(umid.value * 10.0)     : HIST_INVALID;
    rec.press_x10 = pressao.valid  ? (int16_t)round(pressao.value * 10.0) : HIST_INVALID;
    rec.alt_m     = altitude.valid ? (int16_t)round(altitude.value)        : HIST_INVALID;
    rec.gas       = (uint16_t)gas;
    rec.chuva     = (uint16_t)chuva;

    histGravarRegistro(rec);

    String resumo = "Amostra #" + String(histCount) + "/" + String(histCapacity) + " — ";
    resumo += temp.valid ? ("T=" + String(temp.value, 1) + "C ") : "T=-- ";
    resumo += umid.valid ? ("U=" + String(umid.value, 1) + "% ") : "U=-- ";
    resumo += pressao.valid ? ("P=" + String(pressao.value, 1) + "hPa ") : "P=-- ";
    resumo += "Gas=" + String(gas) + " Chuva=" + String(chuva);
    logInfo(resumo);
}

void histApagarTudo() {
    LittleFS.remove(HIST_FILE);
    File f = LittleFS.open(HIST_FILE, "w");
    if (f) f.close();
    histWriteIndex = 0;
    histCount = 0;
    salvarHistMeta();
    logInfo("Histórico do dispositivo apagado pelo usuário");
}

// ::: Funções auxiliares

String badge(String status) {
    if (status == "ok")    return "<span class=\"sensor-badge badge-ok\">ok</span>";
    if (status == "nan")   return "<span class=\"sensor-badge badge-nan\">nan</span>";
    if (status == "range") return "<span class=\"sensor-badge badge-range\">fora de range</span>";
    return "<span class=\"sensor-badge badge-offline\">offline</span>";
}

// ::: Handlers do Servidor

void handle_JSONData() {
    SensorReading temp     = readDHT(false);
    SensorReading umid     = readDHT(true);
    SensorReading pressao  = readBMPPressure();
    SensorReading altitude = readBMPAltitude();

    int chuva = analogRead(RAIN_SENSOR_PIN);
    int gas   = analogRead(MQ135_PIN);

    // Buffer fixo em vez de concatenação repetida de String
    // (JSON pequeno, mas já ajuda a não fragmentar o heap).
    char json[400];
    char tempBuf[16], umidBuf[16], pressBuf[16], altBuf[16];

    if (temp.valid) dtostrf(temp.value, 0, 2, tempBuf); else strcpy(tempBuf, "null");
    if (umid.valid) dtostrf(umid.value, 0, 2, umidBuf); else strcpy(umidBuf, "null");
    if (pressao.valid) dtostrf(pressao.value, 0, 2, pressBuf); else strcpy(pressBuf, "null");
    if (altitude.valid) dtostrf(altitude.value, 0, 2, altBuf); else strcpy(altBuf, "null");

    String dataHora = getFormattedTime();
    String chuvaTxt = classificarChuva(chuva);

    snprintf(json, sizeof(json),
        "{\"status\":\"%s\",\"data_hora\":\"%s\","
        "\"temperatura\":%s,\"temperatura_status\":\"%s\","
        "\"umidade\":%s,\"umidade_status\":\"%s\","
        "\"pressao\":%s,\"pressao_status\":\"%s\","
        "\"altitude\":%s,\"altitude_status\":\"%s\","
        "\"qualidade_ar\":%d,\"qualidade_ar_status\":\"ok\","
        "\"chuva_valor\":%d,\"chuva\":\"%s\"}",
        WiFi.status() == WL_CONNECTED ? "Online" : "Offline",
        dataHora.c_str(),
        tempBuf, temp.status.c_str(),
        umidBuf, umid.status.c_str(),
        pressBuf, pressao.status.c_str(),
        altBuf, altitude.status.c_str(),
        gas,
        chuva, chuvaTxt.c_str());

    server.send(200, "application/json", json);
}

void handle_Logs() {
    server.send(200, "text/plain; charset=utf-8", errorLog);
}

void handle_HistData() {
    if (!histReady) {
        server.send(503, "application/json", "{\"erro\":\"historico indisponivel\"}");
        return;
    }

    uint32_t limite = histCount;
    if (server.hasArg("limit")) {
        long l = server.arg("limit").toInt();
        if (l > 0 && (uint32_t)l < limite) limite = (uint32_t)l;
    }
    const uint32_t MAX_RETORNO = 1500; // protege heap do ESP e o navegador
    if (limite > MAX_RETORNO) limite = MAX_RETORNO;

    File f = LittleFS.open(HIST_FILE, "r");
    if (!f) {
        server.send(500, "application/json", "{\"erro\":\"falha ao abrir historico\"}");
        return;
    }

    // Resposta enviada em pedaços (chunked), igual ao HTML — nunca
    // monta o JSON inteiro numa String só na RAM.
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");

    char cab[100];
    snprintf(cab, sizeof(cab),
        "{\"capacidade\":%lu,\"registros\":%lu,\"retornados\":%lu,\"dados\":[",
        (unsigned long)histCapacity, (unsigned long)histCount, (unsigned long)limite);
    server.sendContent(cab);

    uint32_t oldestIdx = (histCount < histCapacity) ? 0 : histWriteIndex;
    uint32_t inicioLogico = histCount - limite;

    char tBuf[8], uBuf[8], pBuf[8], aBuf[8];
    char item[130];
    for (uint32_t i = 0; i < limite; i++) {
        uint32_t logico = inicioLogico + i;
        uint32_t fisico = (oldestIdx + logico) % histCapacity;

        HistRecord rec;
        f.seek((uint32_t)fisico * sizeof(HistRecord), SeekSet);
        f.read((uint8_t*)&rec, sizeof(HistRecord));

        if (rec.temp_x10  == HIST_INVALID) strcpy(tBuf, "null"); else dtostrf(rec.temp_x10  / 10.0, 0, 1, tBuf);
        if (rec.umid_x10  == HIST_INVALID) strcpy(uBuf, "null"); else dtostrf(rec.umid_x10  / 10.0, 0, 1, uBuf);
        if (rec.press_x10 == HIST_INVALID) strcpy(pBuf, "null"); else dtostrf(rec.press_x10 / 10.0, 0, 1, pBuf);
        if (rec.alt_m     == HIST_INVALID) strcpy(aBuf, "null"); else snprintf(aBuf, sizeof(aBuf), "%d", rec.alt_m);

        snprintf(item, sizeof(item),
            "%s{\"t\":%lu,\"temp\":%s,\"umid\":%s,\"press\":%s,\"alt\":%s,\"gas\":%u,\"chuva\":%u}",
            (i == 0 ? "" : ","), (unsigned long)rec.epoch, tBuf, uBuf, pBuf, aBuf, rec.gas, rec.chuva);
        server.sendContent(item);
    }
    server.sendContent("]}");
    f.close();
}

void handle_HistClear() {
    if (!histReady) {
        server.send(503, "application/json", "{\"erro\":\"historico indisponivel\"}");
        return;
    }
    histApagarTudo();
    server.send(200, "application/json", "{\"ok\":true}");
}

void handle_NotFound() {
    server.send(404, "text/plain", "Recurso não encontrado");
}

// ::: Blocos de HTML fixos (em FLASH, não em RAM)
// Cada bloco grande fica em PROGMEM e é enviado direto por
// sendContent_P, nunca é copiado inteiro para RAM. Só os
// trechos pequenos e dinâmicos (badges, valores, timestamp,
// config do Gemini) usam String, e são poucos bytes cada.

static const char PAGE_PART1[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Estação Formiga</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=Inter:wght@300;400;500;600;700&display=swap" rel="stylesheet">
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
:root {
  --bg-grad:  linear-gradient(135deg, #eef1ff 0%, #f0f7ff 45%, #eafbfa 100%);
  --surface:  #ffffff;
  --card:     #ffffff;
  --border:   #e3e8f0;
  --accent:   #5b6ee8;
  --accent2:  #22b8cf;
  --teal:     #0d9488;
  --warn:     #d98324;
  --danger:   #dc4b4b;
  --success:  #23a066;
  --text:     #1c2536;
  --sub:      #626e88;
  --muted:    #98a2b8;
  --mono:     'Space Mono', monospace;
  --sans:     'Inter', sans-serif;
  --radius:   12px;
  --header-grad:  linear-gradient(180deg, #ffffff 0%, #f6f8fc 100%);
  --avatar-grad:  linear-gradient(135deg, #5b6ee8, #22b8cf);
  --btn-primary-text: #ffffff;
  --card-shadow: rgba(91,110,232,0.14);
}

[data-theme="dark"] {
  --bg-grad:  linear-gradient(135deg, #0d1b2a 0%, #101f31 50%, #0a1622 100%);
  --surface:  #152236;
  --card:     #1a2d42;
  --border:   #243d57;
  --accent:   #5eabd6;
  --accent2:  #7eb8d4;
  --teal:     #38b2ac;
  --warn:     #e8a838;
  --danger:   #d96060;
  --success:  #4caf82;
  --text:     #dce8f0;
  --sub:      #8aafc8;
  --muted:    #4d6880;
  --header-grad:  linear-gradient(180deg, #11263d 0%, #152236 100%);
  --avatar-grad:  linear-gradient(135deg, #2d6a9f, #38b2ac);
  --btn-primary-text: #0d1b2a;
  --card-shadow: rgba(0,0,0,0.25);
}

*, *::before, *::after { margin: 0; padding: 0; box-sizing: border-box; }

html { color-scheme: light; }
[data-theme="dark"] { color-scheme: dark; }

body {
  background: var(--bg-grad);
  background-attachment: fixed;
  color: var(--text);
  font-family: var(--sans);
  min-height: 100vh;
  transition: background 0.4s ease, color 0.3s ease;
}

/* ::: Header centralizado */
header {
  background: var(--header-grad);
  border-bottom: 1px solid var(--border);
  padding: 40px 24px 30px;
  text-align: center;
  position: relative;
  overflow: hidden;
  transition: background 0.4s ease, border-color 0.4s ease;
}
header::before {
  content: "";
  position: absolute;
  top: 0; left: 0; right: 0;
  height: 3px;
  background: linear-gradient(90deg, transparent, var(--accent), var(--accent2), var(--accent), transparent);
  opacity: 0.85;
}
.header-icon-wrap {
  width: 56px; height: 56px;
  margin: 0 auto 14px;
  border-radius: 16px;
  display: flex; align-items: center; justify-content: center;
  background: linear-gradient(135deg, color-mix(in srgb, var(--accent) 14%, transparent), color-mix(in srgb, var(--accent2) 14%, transparent));
  border: 1px solid color-mix(in srgb, var(--accent) 22%, transparent);
}
.header-icon {
  font-size: 1.8rem;
  display: block;
  filter: drop-shadow(0 0 10px rgba(94,171,214,0.35));
}
header h1 {
  font-family: var(--mono);
  font-size: 1.5rem;
  letter-spacing: 0.16em;
  background: linear-gradient(90deg, var(--accent), var(--accent2));
  -webkit-background-clip: text;
  background-clip: text;
  color: transparent;
  font-weight: 700;
}
header p {
  font-size: 0.78rem;
  color: var(--sub);
  margin-top: 6px;
  font-weight: 400;
  letter-spacing: 0.04em;
}
.header-pills {
  display: flex;
  justify-content: center;
  gap: 10px;
  margin-top: 16px;
  flex-wrap: wrap;
}
.pill {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  border-radius: 999px;
  padding: 4px 13px;
  font-size: 0.7rem;
  font-family: var(--mono);
  border: 1px solid;
}
.pill-live {
  background: color-mix(in srgb, var(--success) 10%, transparent);
  border-color: color-mix(in srgb, var(--success) 30%, transparent);
  color: var(--success);
}
.pill-net {
  background: color-mix(in srgb, var(--accent) 10%, transparent);
  border-color: color-mix(in srgb, var(--accent) 25%, transparent);
  color: var(--accent);
}
.dot {
  width: 6px; height: 6px;
  border-radius: 50%;
  background: currentColor;
  box-shadow: 0 0 5px currentColor;
  animation: pulse 2.2s infinite;
}
@keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.35} }

.theme-toggle {
  position: absolute;
  top: 18px;
  right: 20px;
  width: 34px;
  height: 34px;
  border-radius: 50%;
  background: var(--surface);
  border: 1px solid var(--border);
  color: var(--sub);
  display: inline-flex;
  align-items: center;
  justify-content: center;
  font-size: 0.95rem;
  cursor: pointer;
  padding: 0;
  transition: transform 0.35s ease, border-color 0.2s, background 0.3s ease, box-shadow 0.2s;
}
.theme-toggle:hover {
  border-color: var(--accent);
  color: var(--accent);
  transform: rotate(18deg) scale(1.08);
  box-shadow: 0 0 12px var(--card-shadow);
}

/* ::: Layout */
main { padding: 28px 24px; max-width: 1280px; margin: 0 auto; }

/* ::: Métricas */
.metrics {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  gap: 12px;
  margin-bottom: 26px;
}
.metric-card {
  background: var(--card);
  border: 1px solid var(--border);
  border-top: 2px solid var(--accent);
  border-radius: var(--radius);
  padding: 15px 16px 13px;
  transition: transform 0.18s, box-shadow 0.18s, background 0.35s ease, border-color 0.35s ease;
}
.metric-card:hover {
  transform: translateY(-2px);
  box-shadow: 0 6px 20px var(--card-shadow);
}
.metric-card.temp   { border-top-color: #d96060; }
.metric-card.hum    { border-top-color: #5e9fd6; }
.metric-card.press  { border-top-color: #4caf82; }
.metric-card.alt    { border-top-color: #9b7fd4; }
.metric-card.rain   { border-top-color: #38b2ac; }
.metric-card.air    { border-top-color: #e8a838; }

.metric-label {
  font-size: 0.66rem;
  font-weight: 600;
  letter-spacing: 0.09em;
  text-transform: uppercase;
  color: var(--sub);
  margin-bottom: 9px;
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.sensor-badge {
  font-family: var(--mono);
  font-size: 0.58rem;
  padding: 1px 6px;
  border-radius: 4px;
  text-transform: none;
  letter-spacing: 0;
}
.badge-ok      { background: color-mix(in srgb, var(--success) 14%, transparent); color: var(--success); }
.badge-nan     { background: color-mix(in srgb, var(--danger) 14%, transparent);  color: var(--danger); }
.badge-range   { background: color-mix(in srgb, var(--warn) 14%, transparent);    color: var(--warn); }
.badge-offline { background: color-mix(in srgb, var(--muted) 18%, transparent);   color: var(--muted); }

.metric-value {
  font-family: var(--mono);
  font-size: 1.8rem;
  font-weight: 700;
  line-height: 1;
  color: var(--text);
}
.metric-value.invalid { color: var(--muted); font-size: 1.4rem; }
.metric-sub { font-size: 0.7rem; color: var(--sub); margin-top: 4px; }

/* ::: Gráficos */
.charts {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
  margin-bottom: 26px;
}
.chart-card {
  background: var(--card);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 18px 20px;
  transition: background 0.35s ease, border-color 0.35s ease;
}
.chart-card.full { grid-column: 1 / -1; }
.chart-title {
  font-size: 0.68rem;
  font-weight: 600;
  letter-spacing: 0.09em;
  text-transform: uppercase;
  color: var(--sub);
  margin-bottom: 14px;
}
canvas { width: 100% !important; height: 200px !important; }

/* ::: Rodapé de controles */
.footer-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
  border-top: 1px solid var(--border);
  padding-top: 20px;
  margin-bottom: 0;
}
.timestamp {
  font-family: var(--mono);
  font-size: 0.7rem;
  color: var(--muted);
}
.timestamp span { color: var(--accent); }
.hist-status { font-family: var(--mono); font-size: 0.68rem; color: var(--muted); margin-top: 3px; }
.hist-bar-wrap {
  width: 180px;
  max-width: 100%;
  height: 4px;
  border-radius: 999px;
  background: color-mix(in srgb, var(--muted) 20%, transparent);
  margin-top: 6px;
  overflow: hidden;
}
.hist-bar-fill {
  height: 100%;
  width: 0%;
  border-radius: 999px;
  background: linear-gradient(90deg, var(--accent), var(--accent2));
  transition: width 0.6s ease, background 0.4s ease;
}
.hist-bar-fill.warn { background: linear-gradient(90deg, var(--warn), var(--danger)); }
.error-msg { font-size: 0.7rem; color: var(--danger); display: none; margin-top: 3px; }
.btn-group { display: flex; gap: 9px; flex-wrap: wrap; }

.danger-outline {
  color: var(--muted);
}
.danger-outline:hover {
  border-color: var(--danger) !important;
  color: var(--danger) !important;
  background: color-mix(in srgb, var(--danger) 8%, transparent) !important;
}
.danger-outline:disabled {
  opacity: 0.55;
  cursor: not-allowed;
}

.toast {
  position: fixed;
  left: 50%;
  bottom: 24px;
  transform: translateX(-50%) translateY(16px);
  background: var(--card);
  border: 1px solid var(--border);
  color: var(--text);
  padding: 10px 18px;
  border-radius: 10px;
  font-size: 0.82rem;
  box-shadow: 0 8px 24px var(--card-shadow);
  opacity: 0;
  pointer-events: none;
  transition: opacity 0.25s ease, transform 0.25s ease;
  z-index: 999;
}
.toast.show { opacity: 1; transform: translateX(-50%) translateY(0); }
.toast.error { border-color: var(--danger); color: var(--danger); }

button {
  background: transparent;
  border: 1px solid var(--border);
  color: var(--sub);
  padding: 7px 15px;
  border-radius: 8px;
  cursor: pointer;
  font-size: 0.76rem;
  font-family: var(--sans);
  font-weight: 500;
  display: flex;
  align-items: center;
  gap: 6px;
  transition: border-color 0.18s, background 0.18s, color 0.18s;
}
button:hover {
  border-color: var(--accent);
  background: color-mix(in srgb, var(--accent) 7%, transparent);
  color: var(--accent);
}
button.primary {
  background: var(--accent);
  color: var(--btn-primary-text);
  border-color: var(--accent);
  font-weight: 600;
}
button.primary:hover {
  background: var(--accent2);
  border-color: var(--accent2);
  color: var(--btn-primary-text);
}

/* ::: Chat da Ana */
.ana-section {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  overflow: hidden;
  background: var(--card);
  transition: background 0.35s ease, border-color 0.35s ease;
}
.ana-header {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 14px 18px;
  background: linear-gradient(90deg, color-mix(in srgb, var(--accent) 8%, transparent), transparent);
  border-bottom: 1px solid var(--border);
  cursor: pointer;
  user-select: none;
  transition: border-color 0.35s ease;
}
.ana-avatar {
  width: 40px; height: 40px;
  border-radius: 50%;
  display: flex; align-items: center; justify-content: center;
  overflow: hidden;
  flex-shrink: 0;
  box-shadow: 0 0 0 1px color-mix(in srgb, var(--accent) 25%, transparent), 0 0 14px var(--card-shadow);
  transition: box-shadow 0.35s ease;
}
.ana-avatar.thinking {
  animation: avatarPulse 1.4s ease-in-out infinite;
}
@keyframes avatarPulse {
  0%, 100% { box-shadow: 0 0 0 1px color-mix(in srgb, var(--accent) 25%, transparent), 0 0 10px var(--card-shadow); }
  50% { box-shadow: 0 0 0 3px color-mix(in srgb, var(--accent2) 40%, transparent), 0 0 20px var(--card-shadow); }
}
.ana-info h3 {
  font-size: 0.88rem;
  font-weight: 600;
  color: var(--accent2);
}
.ana-info p {
  font-size: 0.68rem;
  color: var(--sub);
  margin-top: 1px;
}
.ana-clear-btn {
  margin-left: auto;
  background: transparent;
  border: 1px solid var(--border);
  color: var(--muted);
  width: 26px;
  height: 26px;
  border-radius: 8px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  font-size: 0.76rem;
  cursor: pointer;
  padding: 0;
  flex-shrink: 0;
  transition: border-color 0.15s, color 0.15s, background 0.15s;
}
.ana-clear-btn:hover {
  border-color: var(--danger);
  color: var(--danger);
  background: color-mix(in srgb, var(--danger) 8%, transparent);
}
.ana-toggle {
  font-size: 0.7rem;
  color: var(--muted);
  font-family: var(--mono);
  flex-shrink: 0;
}

.ana-body { padding: 0 16px 16px; }

.chat-messages {
  height: 280px;
  overflow-y: auto;
  padding: 14px 4px 4px;
  display: flex;
  flex-direction: column;
  gap: 10px;
  scrollbar-width: thin;
  scrollbar-color: var(--border) transparent;
}
.chat-messages::-webkit-scrollbar { width: 4px; }
.chat-messages::-webkit-scrollbar-track { background: transparent; }
.chat-messages::-webkit-scrollbar-thumb { background: var(--border); border-radius: 2px; }

.bubble {
  max-width: 82%;
  padding: 9px 13px;
  border-radius: 14px;
  font-size: 0.82rem;
  line-height: 1.5;
  word-break: break-word;
  transition: background 0.3s ease, border-color 0.3s ease, color 0.25s ease;
}
.bubble.ana {
  align-self: flex-start;
  background: color-mix(in srgb, var(--accent) 10%, transparent);
  border: 1px solid color-mix(in srgb, var(--accent) 22%, transparent);
  color: var(--text);
  border-bottom-left-radius: 4px;
}
.bubble.user {
  align-self: flex-end;
  background: color-mix(in srgb, var(--accent) 18%, transparent);
  border: 1px solid color-mix(in srgb, var(--accent) 32%, transparent);
  color: var(--text);
  border-bottom-right-radius: 4px;
}
.bubble.thinking {
  align-self: flex-start;
  background: color-mix(in srgb, var(--accent) 6%, transparent);
  border: 1px solid color-mix(in srgb, var(--accent) 22%, transparent);
  color: var(--muted);
  font-style: italic;
  font-size: 0.76rem;
  padding: 11px 15px;
}
.typing-dots {
  display: inline-flex;
  align-items: center;
  gap: 5px;
}
.typing-dots span {
  width: 6px; height: 6px;
  border-radius: 50%;
  background: linear-gradient(135deg, var(--accent), var(--accent2));
  animation: typingDot 1.2s ease-in-out infinite;
}
.typing-dots span:nth-child(2) { animation-delay: 0.15s; }
.typing-dots span:nth-child(3) { animation-delay: 0.3s; }
@keyframes typingDot {
  0%, 60%, 100% { transform: translateY(0); opacity: 0.5; }
  30% { transform: translateY(-4px); opacity: 1; }
}
.type-cursor {
  display: inline-block;
  width: 2px;
  height: 12px;
  background: var(--accent);
  margin-left: 2px;
  vertical-align: middle;
  animation: blinkCursor 0.9s steps(1) infinite;
}
@keyframes blinkCursor { 0%, 50% { opacity: 1; } 51%, 100% { opacity: 0; } }

.bubble strong { color: var(--accent); font-weight: 700; }
.bubble em { font-style: italic; }
.bubble code {
  font-family: var(--mono);
  font-size: 0.78em;
  background: color-mix(in srgb, var(--accent) 12%, transparent);
  border-radius: 4px;
  padding: 1px 5px;
}
.bubble pre.md-code {
  font-family: var(--mono);
  font-size: 0.76em;
  background: color-mix(in srgb, var(--accent) 8%, transparent);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 8px 10px;
  margin: 6px 0;
  overflow-x: auto;
  white-space: pre-wrap;
  word-break: break-word;
}

.quick-btns {
  display: flex;
  gap: 7px;
  flex-wrap: wrap;
  margin: 10px 0 10px;
}
.quick-btn {
  background: transparent;
  border: 1px solid var(--border);
  color: var(--sub);
  padding: 5px 11px;
  border-radius: 999px;
  font-size: 0.72rem;
  cursor: pointer;
  font-family: var(--sans);
  transition: border-color 0.15s, background 0.15s, color 0.15s;
}
.quick-btn:hover {
  border-color: var(--accent);
  color: var(--accent);
  background: color-mix(in srgb, var(--accent) 7%, transparent);
}

.chat-input-row {
  display: flex;
  gap: 8px;
  margin-top: 4px;
}
.chat-input {
  flex: 1;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 8px 12px;
  font-size: 0.8rem;
  font-family: var(--sans);
  color: var(--text);
  outline: none;
  transition: border-color 0.15s, background 0.3s ease, color 0.25s ease;
}
.chat-input:focus { border-color: var(--accent); }
.chat-input::placeholder { color: var(--muted); }
.chat-send {
  background: var(--accent);
  border: none;
  color: var(--btn-primary-text);
  padding: 8px 16px;
  border-radius: 8px;
  cursor: pointer;
  font-size: 0.8rem;
  font-weight: 600;
  font-family: var(--sans);
  transition: background 0.15s;
}
.chat-send:hover { background: var(--accent2); }

/* ::: Responsivo */
@media (max-width: 700px) {
  header { padding: 28px 16px 22px; }
  main { padding: 16px 14px; }
  .charts { grid-template-columns: 1fr; }
  .chart-card.full { grid-column: 1; }
  .metric-value { font-size: 1.5rem; }
  canvas { height: 170px !important; }
  .chat-messages { height: 220px; }
}
</style>

<script>
// ::: Configuração Gemini (injetada pelo ESP) ──
)rawliteral";

// Depois deste bloco, o handler injeta as linhas dinâmicas:
//   const ANA_ENABLED = true/false;
//   const GEMINI_KEY = "...";
//   const GEMINI_MODEL = "...";
//   const GEMINI_FALLBACK = "...";

static const char PAGE_PART2[] PROGMEM = R"rawliteral(
// ::: Estado
const charts = {};
const hist = { labels:[], temp:[], umid:[], press:[], alt:[], gas:[], chuva:[] };
let histCapacidadeAtual = null;
let lastSensorData = null;
let chatOpen = true;
let chatHistory = []; // histórico de mensagens para contexto

const BADGE = {
  ok:      ['badge-ok','ok'],
  nan:     ['badge-nan','nan'],
  range:   ['badge-range','fora de range'],
  offline: ['badge-offline','offline']
};

function badge(status) {
  const [cls, label] = BADGE[status] || ['badge-offline','?'];
  return `<span class="sensor-badge ${cls}">${label}</span>`;
}

// ::: Tema claro/escuro
function themeColor(varName) {
  return getComputedStyle(document.documentElement).getPropertyValue(varName).trim();
}

function updateThemeIcon(theme) {
  const btn = document.getElementById('theme-toggle');
  if (btn) btn.textContent = theme === 'light' ? '🌙' : '☀️';
}

function initTheme() {
  const saved = localStorage.getItem('estacao-theme') || 'light';
  document.documentElement.setAttribute('data-theme', saved);
  updateThemeIcon(saved);
}

function toggleTheme() {
  const atual = document.documentElement.getAttribute('data-theme') || 'light';
  const novo = atual === 'light' ? 'dark' : 'light';
  document.documentElement.setAttribute('data-theme', novo);
  localStorage.setItem('estacao-theme', novo);
  updateThemeIcon(novo);
  refreshChartTheme();
}

function refreshChartTheme() {
  const sub = themeColor('--sub');
  const muted = themeColor('--muted');
  const border = themeColor('--border');
  Object.values(charts).forEach(ch => {
    if (ch.options.plugins && ch.options.plugins.legend) ch.options.plugins.legend.labels.color = sub;
    ['x','y'].forEach(eixo => {
      const esc = ch.options.scales && ch.options.scales[eixo];
      if (esc) {
        esc.ticks.color = muted;
        esc.grid.color = border;
        if (esc.title) esc.title.color = muted;
      }
    });
    ch.update();
  });
}

function classAr(v) {
  if (v <= 200) return '🌿 Excelente';
  if (v <= 400) return '😊 Boa';
  if (v <= 600) return '😐 Moderada';
  if (v <= 800) return '😷 Ruim';
  return '🚨 Péssima';
}

// ::: Gráficos
function initChart(id, type, datasets, yLabel) {
  const ctx = document.getElementById(id).getContext('2d');
  const sub = themeColor('--sub');
  const muted = themeColor('--muted');
  const border = themeColor('--border');
  charts[id] = new Chart(ctx, {
    type,
    data: { labels: [], datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 300 },
      plugins: { legend: { labels: { color: sub, font: { family: "'Inter'", size: 11 } } } },
      scales: {
        x: { ticks: { color: muted, maxTicksLimit: 6, font: { size: 10 } }, grid: { color: border } },
        y: { ticks: { color: muted }, grid: { color: border },
             title: { display: true, text: yLabel, color: muted, font: { size: 11 } } }
      }
    }
  });
}

function initCharts() {
  initChart('climateChart', 'line', [
    { label: 'Temperatura (°C)', data: [], borderColor: '#d96060',
      backgroundColor: 'rgba(217,96,96,0.07)', tension: 0.35, fill: true, pointRadius: 2.5 },
    { label: 'Umidade (%)', data: [], borderColor: '#5e9fd6',
      backgroundColor: 'rgba(94,159,214,0.07)', tension: 0.35, fill: true, pointRadius: 2.5 }
  ], 'Valor');

  initChart('pressChart', 'line', [
    { label: 'Pressão (hPa)', data: [], borderColor: '#4caf82',
      backgroundColor: 'rgba(76,175,130,0.07)', tension: 0.35, fill: true, pointRadius: 2.5 }
  ], 'hPa');

  initChart('airChart', 'bar', [
    { label: 'Qualidade do Ar (ppm)', data: [],
      backgroundColor: 'rgba(232,168,56,0.55)', borderColor: '#e8a838', borderWidth: 1 }
  ], 'ppm');
}

function fmt(v, dec, unit) {
  return (v !== null && v !== undefined) ? v.toFixed(dec) + ' ' + unit : null;
}

function setMetric(id, valueStr, status) {
  const card = document.getElementById(id);
  if (!card) return;
  const bw = card.querySelector('.badge-wrap');
  if (bw) bw.innerHTML = badge(status);
  const vEl = card.querySelector('.metric-value');
  if (valueStr !== null) {
    vEl.textContent = valueStr;
    vEl.classList.remove('invalid');
  } else {
    vEl.textContent = '—';
    vEl.classList.add('invalid');
  }
}

function pushHist(d) {
  const now = new Date();
  const ts = `${now.getHours().toString().padStart(2,'0')}:${now.getMinutes().toString().padStart(2,'0')}:${now.getSeconds().toString().padStart(2,'0')}`;
  hist.labels.push(ts);
  hist.temp.push(d.temperatura);
  hist.umid.push(d.umidade);
  hist.press.push(d.pressao);
  hist.alt.push(d.altitude);
  hist.gas.push(d.qualidade_ar);
  hist.chuva.push(d.chuva_valor ?? null);
}

function updateCharts() {
  charts.climateChart.data.labels = hist.labels;
  charts.climateChart.data.datasets[0].data = hist.temp;
  charts.climateChart.data.datasets[1].data = hist.umid;
  charts.climateChart.update();
  charts.pressChart.data.labels = hist.labels;
  charts.pressChart.data.datasets[0].data = hist.press;
  charts.pressChart.update();
  charts.airChart.data.labels = hist.labels;
  charts.airChart.data.datasets[0].data = hist.gas;
  charts.airChart.update();
}

function atualizarDados() {
  fetch('/dados')
    .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
    .then(d => {
      lastSensorData = d;
      pushHist(d);
      updateCharts();

      setMetric('m-temp',  fmt(d.temperatura, 1, 'ºC'), d.temperatura_status);
      setMetric('m-umid',  fmt(d.umidade,     1, '%'),  d.umidade_status);
      setMetric('m-press', fmt(d.pressao,     1, 'hPa'),d.pressao_status);
      setMetric('m-alt',   fmt(d.altitude,    1, 'm'),  d.altitude_status);

      const arEl = document.getElementById('m-air');
      if (arEl) {
        arEl.querySelector('.metric-value').textContent = d.qualidade_ar + ' ppm';
        const bw = arEl.querySelector('.badge-wrap');
        if (bw) bw.innerHTML = badge('ok');
        const ms = arEl.querySelector('.metric-sub');
        if (ms) ms.textContent = classAr(d.qualidade_ar);
      }

      const rEl = document.getElementById('m-rain');
      if (rEl) {
        rEl.querySelector('.metric-value').textContent = d.chuva;
        const bw = rEl.querySelector('.badge-wrap');
        if (bw) bw.innerHTML = badge('ok');
      }

      const ts = document.getElementById('ts');
      if (ts) ts.innerHTML = 'Última atualização: <span>' + d.data_hora + '</span>';

      const ns = document.getElementById('net-status');
      if (ns) {
        ns.textContent = d.status === 'Online' ? '● Online' : '● Offline';
        ns.style.color = d.status === 'Online' ? 'var(--success)' : 'var(--danger)';
      }

      const em = document.getElementById('err-msg');
      if (em) em.style.display = 'none';
    })
    .catch(e => {
      const em = document.getElementById('err-msg');
      if (em) { em.style.display = 'block'; em.textContent = '⚠ Falha na atualização — ' + new Date().toLocaleTimeString(); }
      console.error(e);
    });
}

function exportCSV() {
  const rows = ['Hora,Temperatura,Umidade,Pressao,Altitude,QualidadeAr'];
  hist.labels.forEach((h, i) => {
    rows.push([h, hist.temp[i]??'', hist.umid[i]??'', hist.press[i]??'', hist.alt[i]??'', hist.gas[i]??''].join(','));
  });
  const a = document.createElement('a');
  a.href = 'data:text/csv;charset=utf-8,' + encodeURIComponent(rows.join('\n'));
  a.download = 'estacao_' + new Date().toISOString().slice(0,19).replace(/:/g,'-') + '.csv';
  a.click();
}

// ::: Histórico persistente (gravado na flash do ESP8266)
function formatarEpoch(t) {
  if (!t) return '--:--:--';
  const dt = new Date(t * 1000);
  return dt.getHours().toString().padStart(2,'0') + ':' +
         dt.getMinutes().toString().padStart(2,'0') + ':' +
         dt.getSeconds().toString().padStart(2,'0');
}

function atualizarInfoHistorico(registros, capacidade) {
  const el = document.getElementById('hist-info');
  const bar = document.getElementById('hist-bar-fill');
  if (!el) return;
  if (registros === null) {
    el.textContent = 'Histórico salvo no dispositivo: indisponível';
    if (bar) bar.style.width = '0%';
    return;
  }
  const pct = capacidade ? Math.round((registros / capacidade) * 100) : 0;
  el.textContent = `Histórico salvo no dispositivo: ${registros} / ${capacidade} amostras (${pct}%)`;
  if (bar) {
    bar.style.width = pct + '%';
    bar.classList.toggle('warn', pct >= 85);
  }
}

async function carregarHistorico() {
  try {
    const r = await fetch('/historico?limit=1000');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const d = await r.json();

    histCapacidadeAtual = d.capacidade;
    d.dados.forEach(reg => {
      hist.labels.push(formatarEpoch(reg.t));
      hist.temp.push(reg.temp);
      hist.umid.push(reg.umid);
      hist.press.push(reg.press);
      hist.alt.push(reg.alt);
      hist.gas.push(reg.gas);
      hist.chuva.push(reg.chuva);
    });
    updateCharts();
    atualizarInfoHistorico(d.registros, d.capacidade);
  } catch (e) {
    console.warn('Histórico do dispositivo indisponível:', e);
    atualizarInfoHistorico(null, null);
  }
}

function mostrarToast(msg, erro) {
  let t = document.getElementById('toast');
  if (!t) {
    t = document.createElement('div');
    t.id = 'toast';
    document.body.appendChild(t);
  }
  t.textContent = msg;
  t.className = 'toast show' + (erro ? ' error' : '');
  clearTimeout(t._timer);
  t._timer = setTimeout(() => { t.className = 'toast'; }, 3200);
}

async function limparHistoricoDispositivo() {
  if (!confirm('Isso apaga permanentemente o histórico salvo no ESP8266 (os gráficos desta sessão continuam até você recarregar a página). Deseja continuar?')) return;

  const btn = document.getElementById('hist-clear-btn');
  if (btn) { btn.disabled = true; btn.textContent = '⏳ Apagando...'; }

  try {
    const r = await fetch('/historico/limpar', { method: 'POST' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    atualizarInfoHistorico(0, histCapacidadeAtual);
    mostrarToast('Histórico do dispositivo apagado ✅');
  } catch (e) {
    mostrarToast('Falha ao apagar o histórico ⚠️', true);
    console.error(e);
  } finally {
    if (btn) { btn.disabled = false; btn.textContent = '🗑 Apagar histórico salvo'; }
  }
}

// ::: Chat da Ana
function toggleChat() {
  chatOpen = !chatOpen;
  const body = document.getElementById('ana-body');
  const tog  = document.getElementById('ana-toggle');
  if (body) body.style.display = chatOpen ? '' : 'none';
  if (tog)  tog.textContent = chatOpen ? '▲ recolher' : '▼ expandir';
}

function limparChat() {
  const msgs = document.getElementById('chat-messages');
  if (!msgs || !msgs.hasChildNodes()) return;
  if (!confirm('Limpar toda a conversa com a Ana?')) return;
  msgs.innerHTML = '';
  chatHistory = [];
  if (ANA_ENABLED) {
    typeBubble('Conversa limpa. Em que posso ajudar agora? 🌤️', 'ana');
  }
}

function buildSensorContext() {
  if (!lastSensorData) return 'Ainda aguardando leitura dos sensores.';
  const d = lastSensorData;
  const linhas = [
    `Horário: ${d.data_hora}`,
    `Temperatura: ${d.temperatura !== null ? d.temperatura.toFixed(1) + ' ºC' : 'indisponível (sensor: ' + d.temperatura_status + ')'}`,
    `Umidade relativa: ${d.umidade !== null ? d.umidade.toFixed(1) + ' %' : 'indisponível (sensor: ' + d.umidade_status + ')'}`,
    `Pressão atmosférica: ${d.pressao !== null ? d.pressao.toFixed(1) + ' hPa' : 'indisponível (sensor: ' + d.pressao_status + ')'}`,
    `Altitude estimada: ${d.altitude !== null ? d.altitude.toFixed(1) + ' m' : 'indisponível'}`,
    `Qualidade do ar (MQ135): ${d.qualidade_ar} ppm`,
    `Precipitação: ${d.chuva}`,
    `Status de rede: ${d.status}`
  ];
  return linhas.join('\n');
}

function addBubble(text, role) {
  const msgs = document.getElementById('chat-messages');
  if (!msgs) return;
  const div = document.createElement('div');
  div.className = 'bubble ' + role;
  div.textContent = text;
  msgs.appendChild(div);
  msgs.scrollTop = msgs.scrollHeight;
  return div;
}

function addTypingIndicator() {
  const msgs = document.getElementById('chat-messages');
  if (!msgs) return null;
  const div = document.createElement('div');
  div.className = 'bubble thinking';
  div.id = 'bubble-thinking';
  div.innerHTML = '<span class="typing-dots"><span></span><span></span><span></span></span>';
  msgs.appendChild(div);
  msgs.scrollTop = msgs.scrollHeight;
  const av = document.getElementById('ana-avatar');
  if (av) av.classList.add('thinking');
  return div;
}

function removeThinking() {
  const t = document.getElementById('bubble-thinking');
  if (t) t.remove();
  const av = document.getElementById('ana-avatar');
  if (av) av.classList.remove('thinking');
}

// Converte um subconjunto seguro de Markdown (negrito, itálico, código, quebras
// de linha e listas simples) em HTML. Escapa entidades HTML antes de tudo,
// para nunca injetar marcação vinda da resposta da IA.
function renderMarkdown(text) {
  let out = String(text)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');

  // Bloco de código ```...```
  out = out.replace(/```([\s\S]*?)```/g, (_, code) => '<pre class="md-code">' + code.trim() + '</pre>');
  // Código inline `...`
  out = out.replace(/`([^`\n]+)`/g, '<code>$1</code>');
  // Negrito **texto** ou __texto__ (processado antes do itálico)
  out = out.replace(/\*\*([^\n*]+)\*\*/g, '<strong>$1</strong>');
  out = out.replace(/__([^\n_]+)__/g, '<strong>$1</strong>');
  // Itálico *texto* ou _texto_ (o que sobrar depois do negrito)
  out = out.replace(/\*([^\n*]+)\*/g, '<em>$1</em>');
  out = out.replace(/(^|[^\w_])_([^\n_]+)_(?!\w)/g, '$1<em>$2</em>');
  // Listas simples "- item" ou "* item" no início da linha
  out = out.replace(/^[-*]\s+(.+)$/gm, '• $1');
  // Quebras de linha
  out = out.replace(/\n/g, '<br>');
  return out;
}

// Anima a resposta da Ana aparecendo palavra por palavra, já renderizando
// Markdown (negrito, itálico, código) conforme o texto vai surgindo.
function typeBubble(text, role) {
  const msgs = document.getElementById('chat-messages');
  if (!msgs) return Promise.resolve();
  const div = document.createElement('div');
  div.className = 'bubble ' + role + ' typing';
  msgs.appendChild(div);
  msgs.scrollTop = msgs.scrollHeight;

  const palavras = text.split(' ');
  let i = 0;
  let acumulado = '';
  return new Promise(resolve => {
    function passo() {
      if (i < palavras.length) {
        acumulado += (i === 0 ? '' : ' ') + palavras[i];
        div.innerHTML = renderMarkdown(acumulado) + '<span class="type-cursor"></span>';
        i++;
        msgs.scrollTop = msgs.scrollHeight;
        setTimeout(passo, 28 + Math.random() * 42);
      } else {
        div.classList.remove('typing');
        div.innerHTML = renderMarkdown(acumulado);
        resolve(div);
      }
    }
    passo();
  });
}

async function askAna(userText) {
  if (!ANA_ENABLED) return;
  if (!userText.trim()) return;

  addBubble(userText, 'user');
  chatHistory.push({ role: 'user', parts: [{ text: userText }] });

  addTypingIndicator();

  const sensorCtx = buildSensorContext();
  const systemInstruction = `Você é Ana, assistente meteorológica simpática e direta da Estação Formiga, uma estação IoT caseira feita com ESP8266, DHT11, BMP180 e MQ135. Responda sempre em português do Brasil, de forma clara e amigável. Use os dados dos sensores quando relevante. Seja concisa, mas completa. Não invente dados que não estão disponíveis.\n\nDados atuais da estação:\n${sensorCtx}`;

  // Monta o payload com histórico (máx últimas 6 trocas)
  const recentHistory = chatHistory.slice(-12);
  const payload = {
    system_instruction: { parts: [{ text: systemInstruction }] },
    contents: recentHistory
  };

  const url = `https://generativelanguage.googleapis.com/v1beta/models/${GEMINI_MODEL}:generateContent?key=${GEMINI_KEY}`;

  try {
    let res = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });

    // Fallback de modelo se o primário falhar
    if (!res.ok && GEMINI_FALLBACK) {
      const urlFb = `https://generativelanguage.googleapis.com/v1beta/models/${GEMINI_FALLBACK}:generateContent?key=${GEMINI_KEY}`;
      res = await fetch(urlFb, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
    }

    if (!res.ok) throw new Error('Gemini HTTP ' + res.status);

    const data = await res.json();
    const reply = data?.candidates?.[0]?.content?.parts?.[0]?.text || 'Não consegui processar a resposta.';
    removeThinking();
    await typeBubble(reply, 'ana');
    chatHistory.push({ role: 'model', parts: [{ text: reply }] });

  } catch(e) {
    removeThinking();
    typeBubble('Ops, não consegui me conectar ao servidor de IA. Verifique a chave Gemini e a conexão com a internet.', 'ana');
    console.error('Gemini error:', e);
  }
}

function sendChat() {
  const inp = document.getElementById('chat-input');
  if (!inp) return;
  const text = inp.value.trim();
  if (!text) return;
  inp.value = '';
  askAna(text);
}

window.onload = async function() {
  initTheme();
  initCharts();
  await carregarHistorico();
  atualizarDados();
  setInterval(atualizarDados, 15000);

  // Mensagem de boas-vindas da Ana, animada (só se habilitada)
  if (ANA_ENABLED) {
    setTimeout(() => {
      typeBubble('Olá! Sou a Ana, sua assistente meteorológica 🌤️ Estou conectada aos sensores da Estação Formiga e posso te ajudar a entender as condições ambientais. O que você gostaria de saber?', 'ana');
    }, 800);
  }

  // Enter para enviar no chat
  const inp = document.getElementById('chat-input');
  if (inp) inp.addEventListener('keydown', e => { if (e.key === 'Enter') sendChat(); });
};
</script>
</head>
<body>

<header>
  <button class="theme-toggle" id="theme-toggle" onclick="toggleTheme()" title="Alternar tema" aria-label="Alternar tema claro/escuro">🌙</button>
  <div class="header-icon-wrap"><span class="header-icon">🐜</span></div>
  <h1>ESTAÇÃO FORMIGA</h1>
  <p>Monitor Ambiental IoT &middot; ESP8266 &middot; DHT11 · BMP180 · MQ135</p>
  <div class="header-pills">
    <span class="pill pill-live"><span class="dot"></span>Dados em tempo real</span>
    <span class="pill pill-net" id="net-status">● Online</span>
  </div>
</header>

<main>

)rawliteral";

static const char PAGE_PART2B[] PROGMEM = R"rawliteral(  <!-- Métricas -->
  <div class="metrics">

    <div class="metric-card temp" id="m-temp">
      <div class="metric-label">🌡 Temperatura <span class="badge-wrap">)rawliteral";

static const char PAGE_PART3[] PROGMEM = R"rawliteral(</span></div>
      <div class="metric-value">)rawliteral";

static const char PAGE_PART4[] PROGMEM = R"rawliteral(</div>
    </div>

    <div class="metric-card hum" id="m-umid">
      <div class="metric-label">💧 Umidade <span class="badge-wrap">)rawliteral";

static const char PAGE_PART5[] PROGMEM = R"rawliteral(</span></div>
      <div class="metric-value">)rawliteral";

static const char PAGE_PART6[] PROGMEM = R"rawliteral(</div>
    </div>

    <div class="metric-card press" id="m-press">
      <div class="metric-label">⏬ Pressão atm <span class="badge-wrap"></span></div>
      <div class="metric-value invalid">—</div>
    </div>

    <div class="metric-card alt" id="m-alt">
      <div class="metric-label">🏔 Altitude <span class="badge-wrap"></span></div>
      <div class="metric-value invalid">—</div>
    </div>

    <div class="metric-card air" id="m-air">
      <div class="metric-label">🌫 Qualidade do ar <span class="badge-wrap"></span></div>
      <div class="metric-value">—</div>
      <div class="metric-sub"></div>
    </div>

    <div class="metric-card rain" id="m-rain">
      <div class="metric-label">🌧 Precipitação <span class="badge-wrap"></span></div>
      <div class="metric-value" style="font-size:1.05rem;">—</div>
    </div>

  </div>

  <!-- Gráficos -->
  <div class="charts">
    <div class="chart-card full">
      <div class="chart-title">Temperatura &amp; Umidade</div>
      <canvas id="climateChart"></canvas>
    </div>
    <div class="chart-card">
      <div class="chart-title">Pressão Atmosférica</div>
      <canvas id="pressChart"></canvas>
    </div>
    <div class="chart-card">
      <div class="chart-title">Qualidade do Ar</div>
      <canvas id="airChart"></canvas>
    </div>
  </div>

  <!-- Controles -->
  <div class="footer-bar">
    <div>
      <div class="timestamp" id="ts">Última atualização: <span>)rawliteral";

static const char PAGE_PART7[] PROGMEM = R"rawliteral(</span></div>
      <div class="hist-status" id="hist-info">Histórico salvo no dispositivo: carregando…</div>
      <div class="hist-bar-wrap"><div class="hist-bar-fill" id="hist-bar-fill"></div></div>
      <div class="error-msg" id="err-msg"></div>
    </div>
    <div class="btn-group">
      <button onclick="atualizarDados()" class="primary">🔄 Atualizar</button>
      <button onclick="exportCSV()">📥 Exportar CSV</button>
      <button onclick="window.open('/logs')">📋 Logs</button>
      <button id="hist-clear-btn" class="danger-outline" onclick="limparHistoricoDispositivo()">🗑 Apagar histórico salvo</button>
    </div>
  </div>

)rawliteral";

static const char PAGE_ANA_SECTION[] PROGMEM = R"rawliteral(
  <!-- Chat da Ana -->
  <div class="ana-section" style="margin-bottom:26px;">
    <div class="ana-header" onclick="toggleChat()">
      <div class="ana-avatar" id="ana-avatar">
        <svg viewBox="0 0 48 48" width="100%" height="100%" aria-hidden="true">
          <circle cx="24" cy="24" r="24" fill="url(#anaGrad)"/>
          <defs>
            <linearGradient id="anaGrad" x1="0" y1="0" x2="48" y2="48" gradientUnits="userSpaceOnUse">
              <stop offset="0" stop-color="var(--accent)"/>
              <stop offset="1" stop-color="var(--accent2)"/>
            </linearGradient>
          </defs>
          <path d="M24 10c-5.6 0-9.4 4.3-9.4 9.6 0 2.3.6 4.1 1.5 5.6-2.9 1.1-4.9 2.6-5.9 4.3C8.7 31.8 8 34.6 8 38h32c0-3.4-.7-6.2-2.2-8.5-1-1.7-3-3.2-5.9-4.3.9-1.5 1.5-3.3 1.5-5.6 0-5.3-3.8-9.6-9.4-9.6z" fill="rgba(255,255,255,0.94)"/>
          <path d="M24 10c-5.6 0-9.4 4.3-9.4 9.6 0 1.1.15 2.1.44 3 3.1-.6 5.5-2.2 6.96-4.4 1.7 2.5 4.7 4.1 8.2 4.5.2-1 .3-2 .3-3.1 0-5.3-3.8-9.6-9.4-9.6z" fill="var(--accent)" opacity="0.55"/>
        </svg>
      </div>
      <div class="ana-info">
        <h3>Ana · Assistente Meteorológica</h3>
        <p>Pergunte sobre as condições ambientais da estação</p>
      </div>
      <button class="ana-clear-btn" title="Limpar conversa" aria-label="Limpar conversa" onclick="event.stopPropagation(); limparChat();">🗑</button>
      <span class="ana-toggle" id="ana-toggle">▲ recolher</span>
    </div>
    <div class="ana-body" id="ana-body">
      <div class="chat-messages" id="chat-messages"></div>
      <div class="quick-btns">
        <button class="quick-btn" onclick="askAna('Como está o tempo agora?')">🌡 Como está o tempo agora?</button>
        <button class="quick-btn" onclick="askAna('A qualidade do ar está boa hoje?')">🌫 Qualidade do ar</button>
        <button class="quick-btn" onclick="askAna('Há risco de chuva no momento?')">🌧 Risco de chuva?</button>
      </div>
      <div class="chat-input-row">
        <input class="chat-input" id="chat-input" type="text" placeholder="Pergunte à Ana..." maxlength="300">
        <button class="chat-send" onclick="sendChat()">Enviar</button>
      </div>
    </div>
  </div>
)rawliteral";

static const char PAGE_TAIL[] PROGMEM = R"rawliteral(

</main>
</body>
</html>)rawliteral";

// ::: Handler principal: envia a página em pedaços (chunked)
// Nenhuma "String html" gigante é criada. Cada bloco grande vem
// direto da flash (sendContent_P) e só os trechos pequenos e
// dinâmicos passam por String, um de cada vez.
void handle_OnConnect() {
    SensorReading temp = readDHT(false);
    SensorReading umid = readDHT(true);
    String dataHora = getFormattedTime();

    bool anaEnabled = strlen(GEMINI_KEY) > 0;

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");

    // Bloco 1: DOCTYPE até o comentário de config (CSS inteiro embutido)
    server.sendContent_P(PAGE_PART1);

    // Config dinâmica do Gemini (pequena, poucas dezenas de bytes)
    if (anaEnabled) {
        server.sendContent("const ANA_ENABLED = true;\n");
        server.sendContent("const GEMINI_KEY = \"" + String(GEMINI_KEY) + "\";\n");
        server.sendContent("const GEMINI_MODEL = \"" + String(GEMINI_MODEL) + "\";\n");
        server.sendContent("const GEMINI_FALLBACK = \"" + String(GEMINI_FALLBACK_MODEL) + "\";\n");
    } else {
        server.sendContent("const ANA_ENABLED = false;\n");
        server.sendContent("const GEMINI_KEY = '';\n");
        server.sendContent("const GEMINI_MODEL = '';\n");
        server.sendContent("const GEMINI_FALLBACK = '';\n");
    }

    // Bloco 2: resto do JS + body + header, até a abertura do <main>
    server.sendContent_P(PAGE_PART2);

    // Chat da Ana — agora logo no topo, só se Gemini estiver configurado
    if (anaEnabled) {
        server.sendContent_P(PAGE_ANA_SECTION);
    }

    // Bloco 2B: métricas, até o badge de temperatura
    server.sendContent_P(PAGE_PART2B);
    server.sendContent(badge(temp.valid ? "ok" : "nan"));

    // Bloco 3: até o valor de temperatura
    server.sendContent_P(PAGE_PART3);
    server.sendContent(temp.valid ? (String(temp.value, 1) + " ºC") : "—");

    // Bloco 4: até o badge de umidade
    server.sendContent_P(PAGE_PART4);
    server.sendContent(badge(umid.valid ? "ok" : "nan"));

    // Bloco 5: até o valor de umidade
    server.sendContent_P(PAGE_PART5);
    server.sendContent(umid.valid ? (String(umid.value, 1) + " %") : "—");

    // Bloco 6: resto das métricas, gráficos, footer até o timestamp
    server.sendContent_P(PAGE_PART6);
    server.sendContent(dataHora);

    // Bloco 7: fim do footer
    server.sendContent_P(PAGE_PART7);

    // Bloco final: fecha main/body/html
    server.sendContent_P(PAGE_TAIL);

    // Encerra a resposta chunked
    server.sendContent("");
}
