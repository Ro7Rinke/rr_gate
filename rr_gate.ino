//version 0.10

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Arduino.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>
#include <time.h>
#include "secrets.h"

struct Session {
  String token;
  String username;
  time_t expiry;
  bool active;
};

Session sessions[USER_COUNT];

// const char* ssid = "WIFI_NAME";
// const char* password = "WIFI_PASS";

const int RELAY_PIN = 5;
WebServer server(80);

// Usuário e senha (SHA256 de "1234")
// const String USERNAME = "admin";
// const String PASSWORD_HASH = "03ac674216f3e15c761ee1a5e255f067953623c8b388b4459e13f978d7c846f4";

// Sessão
// String sessionToken = "";
// time_t sessionExpiry = 0;
const time_t SESSION_DURATION = 7UL * 24UL * 60UL * 60UL; // 1 semana

// Pulso do relé
bool relayActive = false;
unsigned long relayStart = 0;
const unsigned long RELAY_DURATION = 500; // 0.5 seg

// Configuração NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600; // GMT-3
const int daylightOffset_sec = 0;
// unsigned long lastNtpSync = 0;
// const unsigned long NTP_SYNC_INTERVAL = 12UL * 60UL * 60UL * 1000UL; // 12h
bool timeSynced = false;
const int MAX_NTP_RETRIES = 5;

// Fallback
time_t fallbackBase = 0;

// ================== HTML ==================
const char* loginPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Login</title>
<style>
* {
  box-sizing: border-box;
}

html, body {
  margin: 0;
  padding: 0;
  height: 100%;
  font-family: sans-serif;
  background: #2a2a2a;
}

body {
  display: flex;
  justify-content: center;
  align-items: center;
  min-height: 100dvh;
  padding: 20px;
}

form {
  width: 100%;
  max-width: 380px;
  background: #333;
  padding: 40px 30px;
  border-radius: 20px;
  box-shadow: 0 10px 25px rgba(0,0,0,0.6);
  display: flex;
  flex-direction: column;
}

input {
  margin: 12px 0;
  padding: 15px;
  font-size: 1.2rem;
  border-radius: 12px;
  border: none;
  outline: none;
}

button {
  margin-top: 25px;
  padding: 18px;
  font-size: 1.5rem;
  border-radius: 50px;
  background: linear-gradient(145deg,#FFD700,#FFC300);
  color: #222;
  border: none;
  cursor: pointer;
  box-shadow: 
    0 10px 20px rgba(0,0,0,0.5),
    inset 0 -6px 15px rgba(0,0,0,0.3),
    inset 0 6px 10px rgba(255,255,255,0.2);
  transition: all 0.2s ease;
}

button:hover {
  transform: scale(1.05);
}

button:active {
  transform: scale(0.95);
}
</style>
</head>
<body>
<form method="POST" action="/login">
<input type="text" name="user" placeholder="User" required>
<input type="password" name="pass" placeholder="Password" required>
<button type="submit">Entrar</button>
</form>
</body>
</html>
)rawliteral";

String relayPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html lang="pt-br">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Controle</title>

<style>
* {
  box-sizing: border-box;
}

html, body {
  margin: 0;
  padding: 0;
  min-height: 100dvh;
  background: #2a2a2a;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
}

body {
  display: flex;
  justify-content: center;
  align-items: center;
  padding: 24px;
}

.container {
  width: 100%;
  max-width: 420px;
  display: flex;
  justify-content: center;
  align-items: center;
}

button {
  width: min(80vw, 320px);
  height: min(80vw, 320px);
  border-radius: 50%;
  border: none;

  background: linear-gradient(145deg, #FFD700, #FFC300);
  color: #222;
  font-size: clamp(2.3rem, 6vw, 3.5rem);
  font-weight: 600;
  cursor: pointer;

  box-shadow:
    0 15px 30px rgba(0,0,0,0.6),
    inset 0 -8px 18px rgba(0,0,0,0.3),
    inset 0 8px 12px rgba(255,255,255,0.25);

  transition: transform 0.15s ease, box-shadow 0.15s ease;
  text-align: center;
  line-height: 1.2;
}

button:hover {
  transform: scale(1.05);
}

button:active {
  transform: scale(0.95);
  box-shadow:
    0 8px 18px rgba(0,0,0,0.5),
    inset 0 -4px 10px rgba(0,0,0,0.3),
    inset 0 4px 8px rgba(255,255,255,0.2);
}
</style>
</head>

<body>

<div class="container">
  <button id="activateBtn">Acionar⚡</button>
</div>

<script>
document.getElementById("activateBtn").addEventListener("click", () => {
  fetch('/pulse')
    .then(r => r.text())
    .then(msg => alert(msg))
    .catch(() => alert("Erro ao comunicar com o dispositivo."));
});
</script>

</body>
</html>
)rawliteral";
}

// ================== FUNÇÕES ==================
String sha256(const String &input) {
  byte hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  char hashStr[65];
  for (int i = 0; i < 32; i++) sprintf(hashStr + i*2, "%02x", hash[i]);
  hashStr[64] = 0;

  return String(hashStr);
}

// ================== NTP ==================
void syncTime() {
  int retries = 0;
  timeSynced = false;

  while (!timeSynced && retries < MAX_NTP_RETRIES) {
    Serial.println("Tentando sincronizar NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      timeSynced = true;
      // lastNtpSync = millis();
      Serial.println("Hora NTP sincronizada!");
      break;
    } else {
      retries++;
      Serial.println("Falha ao sincronizar hora. Retry...");
      yield();
      delay(2000);
    }
  }

  if (!timeSynced) {
    fallbackBase = millis() / 1000UL;
    Serial.println("Usando fallback com millis()");
  }
}

time_t getCurrentTime() {
  if (timeSynced) return time(NULL);
  return fallbackBase + millis() / 1000UL;
}

// ================== LOGIN ==================
bool authenticate(const String& user, const String& pass) {
  String passHash = sha256(pass);

  for (int i = 0; i < USER_COUNT; i++) {
    if (user == USERS[i].username &&
        passHash == USERS[i].passwordHash) {
      return true;
    }
  }
  return false;
}

bool createSession(const String& username, String& outToken) {
  time_t now = getCurrentTime();

  // 1) Limpa sessões expiradas
  for (int i = 0; i < USER_COUNT; i++) {
    if (sessions[i].active && sessions[i].expiry < now) {
      sessions[i].active = false;
    }
  }

  // 2) Procura slot livre
  for (int i = 0; i < USER_COUNT; i++) {
    if (!sessions[i].active) {
      sessions[i].token =
        String((uint32_t)esp_random(), HEX) +
        String((uint32_t)esp_random(), HEX);

      sessions[i].username = username;
      sessions[i].expiry = now + SESSION_DURATION;
      sessions[i].active = true;

      outToken = sessions[i].token;
      return true;
    }
  }

  // 3) Nenhum slot livre → sobrescreve a mais antiga
  int oldestIndex = 0;
  time_t oldestExpiry = sessions[0].expiry;

  for (int i = 1; i < USER_COUNT; i++) {
    if (sessions[i].expiry < oldestExpiry) {
      oldestExpiry = sessions[i].expiry;
      oldestIndex = i;
    }
  }

  // Sobrescreve
  sessions[oldestIndex].token =
    String((uint32_t)esp_random(), HEX) +
    String((uint32_t)esp_random(), HEX);

  sessions[oldestIndex].username = username;
  sessions[oldestIndex].expiry = now + SESSION_DURATION;
  sessions[oldestIndex].active = true;

  outToken = sessions[oldestIndex].token;
  return true;
}

bool isLoggedIn(String* outUser = nullptr) {
  if (!server.hasHeader("Cookie")) return false;

  String cookie = server.header("Cookie");
  int start = cookie.indexOf("session=");
  if (start == -1) return false;

  start += 8;
  int end = cookie.indexOf(";", start);
  String token = (end == -1) ?
    cookie.substring(start) :
    cookie.substring(start, end);

  time_t now = getCurrentTime();

  for (int i = 0; i < USER_COUNT; i++) {
    if (sessions[i].active &&
        sessions[i].token == token &&
        sessions[i].expiry > now) {

      if (outUser) *outUser = sessions[i].username;
      return true;
    }
  }

  return false;
}

// ================== HANDLERS ==================
void handleLogin() {
  if (isLoggedIn()) {
    server.sendHeader("Location", "/relay");
    server.send(303);
    return;
  }

  if (server.method() == HTTP_POST) {
  String user = server.arg("user");
  String pass = server.arg("pass");

  if (authenticate(user, pass)) {
    String newToken;

    if (createSession(user, newToken)) {
      server.sendHeader("Set-Cookie",
        "session=" + newToken +
        "; Max-Age=604800; HttpOnly; SameSite=Strict; Path=/");

      server.sendHeader("Location", "/relay");
      server.send(303);
      return;
    } else {
      server.send(200, "text/html",
        "<h1>Limite de sessões atingido</h1>");
      return;
    }
  } else {
    server.send(200, "text/html",
      "<h1>Credenciais inválidas</h1>");
    return;
  }
}

  server.send(200, "text/html", loginPage);
}

void handleRelay() {
  if (!isLoggedIn()) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
  server.send(200, "text/html", relayPage());
}

void handlePulse() {
  String user;
  if (!isLoggedIn(&user)) {
    server.send(403, "text/plain", "Não autorizado");
    return;
  }

  Serial.println("Portão acionado por: " + user);
  if (!relayActive) {
    relayActive = true;
    relayStart = millis();
    digitalWrite(RELAY_PIN, LOW);
  }
  server.send(200, "text/plain", "Relé acionado!");
}

// ================== SETUP ==================
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.begin(115200);

  for (int i = 0; i < USER_COUNT; i++) {
    sessions[i].active = false;
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    yield();
  }
  Serial.println();
  Serial.println(WiFi.localIP());

  if (MDNS.begin("portao")) {
    Serial.println("mDNS iniciado");
  }

  syncTime();

  server.on("/", handleLogin);
  server.on("/login", handleLogin);
  server.on("/relay", handleRelay);
  server.on("/pulse", handlePulse);

  const char* headerkeys[] = {"Cookie"};
  size_t headerkeyssize = sizeof(headerkeys)/sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);

  server.begin();
}

// ================== LOOP ==================
void loop() {
  server.handleClient();

  if (relayActive && millis() - relayStart >= RELAY_DURATION) {
    digitalWrite(RELAY_PIN, HIGH);
    relayActive = false;
  }

  // if (timeSynced && millis() - lastNtpSync > NTP_SYNC_INTERVAL) {
  //   syncTime();
  // }
}