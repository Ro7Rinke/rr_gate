// version 0.13

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Arduino.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>
#include <time.h>
#include <RCSwitch.h>
#include <Preferences.h>
#include "secrets.h"

#define TOKEN_LEN 16  // 8 bytes hex (uint32_t x2)
#define MAX_PASS_LEN 64
#define MAX_REMOTES 10

struct Session {
  char token[TOKEN_LEN + 1];
  char username[MAX_USERNAME_LEN + 1];
  time_t expiry;
  bool active;
};

Session sessions[USER_COUNT];

Preferences prefs;

uint32_t remotes[MAX_REMOTES];
int remoteCount = 0;
bool learningMode = false;
const int RF_PIN = 12;
RCSwitch rf = RCSwitch();

const int RELAY_PIN = 13;
WebServer server(80);

const time_t SESSION_DURATION = 7UL * 24UL * 60UL * 60UL;

bool relayActive = false;
unsigned long relayStart = 0;
const unsigned long RELAY_DURATION = 500;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;
const int daylightOffset_sec = 0;
bool timeSynced = false;
const int MAX_NTP_RETRIES = 5;
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

bool remoteAuthorized(uint32_t code) {

  for (int i = 0; i < remoteCount; i++) {
    if (remotes[i] == code)
      return true;
  }

  return false;
}

void removeRemote(int id) {

  if (id < 0 || id >= remoteCount)
    return;

  for (int i = id; i < remoteCount - 1; i++) {
    remotes[i] = remotes[i + 1];
  }

  remoteCount--;
}

void saveRemotes() {
  prefs.putInt("count", remoteCount);
  for (int i = 0; i < remoteCount; i++) {
    char key[10];
    sprintf(key, "r%d", i);
    prefs.putULong(key, remotes[i]);
  }
}

void loadRemotes() {
  remoteCount = prefs.getInt("count", 0);
  for (int i = 0; i < remoteCount; i++) {
    char key[10];
    sprintf(key, "r%d", i);
    remotes[i] = prefs.getULong(key, 0);
  }
}

void clearRemotes() {
  prefs.clear();
  remoteCount = 0;
}

// ================= SHA256 =================
void sha256(const char* input, char* outputHex) {
  byte hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char*)input, strlen(input));
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  for (int i = 0; i < 32; i++)
    sprintf(outputHex + i * 2, "%02x", hash[i]);

  outputHex[64] = 0;
}

// ================= NTP =================
void syncTime() {
  int retries = 0;
  timeSynced = false;

  while (!timeSynced && retries < MAX_NTP_RETRIES) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      timeSynced = true;
      break;
    } else {
      retries++;
      delay(2000);
    }
  }

  if (!timeSynced)
    fallbackBase = millis() / 1000UL;
}

time_t getCurrentTime() {
  if (timeSynced) return time(NULL);
  return fallbackBase + millis() / 1000UL;
}

// ================= AUTH =================
bool authenticate(const char* user, const char* pass) {
  char passHash[65];
  sha256(pass, passHash);

  for (int i = 0; i < USER_COUNT; i++) {
    if (strcmp(user, USERS[i].username) == 0 &&
        strcmp(passHash, USERS[i].passwordHash) == 0) {
      return true;
    }
  }
  return false;
}

// ================= SESSION =================
void generateToken(char* outToken) {
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  sprintf(outToken, "%08x%08x", r1, r2);
}

bool createSession(const char* username, char* outToken) {
  time_t now = getCurrentTime();

  for (int i = 0; i < USER_COUNT; i++) {
    if (sessions[i].active && sessions[i].expiry < now)
      sessions[i].active = false;
  }

  for (int i = 0; i < USER_COUNT; i++) {
    if (sessions[i].active &&
        strcmp(sessions[i].username, username) == 0)
      sessions[i].active = false;
  }

  for (int i = 0; i < USER_COUNT; i++) {
    if (!sessions[i].active) {

      generateToken(sessions[i].token);
      strncpy(sessions[i].username, username, MAX_USERNAME_LEN);
      sessions[i].username[MAX_USERNAME_LEN] = 0;
      sessions[i].expiry = now + SESSION_DURATION;
      sessions[i].active = true;

      strcpy(outToken, sessions[i].token);
      return true;
    }
  }

  int oldest = 0;
  for (int i = 1; i < USER_COUNT; i++) {
    if (sessions[i].expiry < sessions[oldest].expiry)
      oldest = i;
  }

  generateToken(sessions[oldest].token);
  strncpy(sessions[oldest].username, username, MAX_USERNAME_LEN);
  sessions[oldest].username[MAX_USERNAME_LEN] = 0;
  sessions[oldest].expiry = now + SESSION_DURATION;
  sessions[oldest].active = true;

  strcpy(outToken, sessions[oldest].token);
  return true;
}

bool isLoggedIn(char* outUser = nullptr) {
  if (!server.hasHeader("Cookie")) return false;

  const String& cookieStr = server.header("Cookie");
  char cookie[256];
  cookieStr.toCharArray(cookie, sizeof(cookie));

  char* pos = strstr(cookie, "session=");
  if (!pos) return false;

  pos += 8;

  char token[TOKEN_LEN + 1];
  int i = 0;
  while (*pos && *pos != ';' && i < TOKEN_LEN) {
    token[i++] = *pos++;
  }
  token[i] = 0;

  time_t now = getCurrentTime();

  for (int j = 0; j < USER_COUNT; j++) {
    if (sessions[j].active &&
        strcmp(sessions[j].token, token) == 0 &&
        sessions[j].expiry > now) {

      if (outUser)
        strcpy(outUser, sessions[j].username);

      return true;
    }
  }

  return false;
}

// ================= HANDLERS =================
void handleLogin() {

  if (isLoggedIn()) {
    server.sendHeader("Location", "/relay");
    server.send(303);
    return;
  }

  if (server.method() == HTTP_POST) {

    char user[MAX_USERNAME_LEN + 1];
    char pass[MAX_PASS_LEN];

    server.arg("user").toCharArray(user, sizeof(user));
    server.arg("pass").toCharArray(pass, sizeof(pass));

    if (authenticate(user, pass)) {

      char newToken[TOKEN_LEN + 1];

      if (createSession(user, newToken)) {

        char cookieHeader[128];
        snprintf(cookieHeader, sizeof(cookieHeader),
                 "session=%s; Max-Age=604800; HttpOnly; SameSite=Strict; Path=/",
                 newToken);

        server.sendHeader("Set-Cookie", cookieHeader);
        server.sendHeader("Location", "/relay");
        server.send(303);
        return;
      }
    }

    server.send(200, "text/html", "<h1>Credenciais inválidas</h1>");
    return;
  }

  server.send(200, "text/html", loginPage);
}

void handleRelay() {
  if (!isLoggedIn()) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
  server.send(200, "text/html", relayPage().c_str());
}

void handlePulse() {

  char user[MAX_USERNAME_LEN + 1];

  if (!isLoggedIn(user)) {
    server.send(403, "text/plain", "Não autorizado");
    return;
  }

  Serial.print("Portão acionado por: ");
  Serial.println(user);

  if (!relayActive) {
    relayActive = true;
    relayStart = millis();
    digitalWrite(RELAY_PIN, LOW);
  }

  server.send(200, "text/plain", "Relé acionado!");
}

void handleLearn() {

  if (!isLoggedIn()) {
    server.send(403, "text/plain", "Nao autorizado");
    return;
  }

  learningMode = true;

  server.send(200, "text/plain",
              "Pressione o botão do controle agora...");
}

void handleClearRemotes() {

  if (!isLoggedIn()) {
    server.send(403, "text/plain", "Nao autorizado");
    return;
  }

  clearRemotes();

  server.send(200, "text/plain", "Controles removidos!");
}

// ================= SETUP =================
void setup() {

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.begin(115200);

  prefs.begin("rf", false);

  loadRemotes();

  rf.enableReceive(digitalPinToInterrupt(RF_PIN));

  for (int i = 0; i < USER_COUNT; i++)
    sessions[i].active = false;

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  if (MDNS.begin("portao")) {}

  syncTime();

  server.on("/", handleLogin);
  server.on("/login", handleLogin);
  server.on("/relay", handleRelay);
  server.on("/pulse", handlePulse);
  server.on("/learn", handleLearn);
  server.on("/remotes/clear", handleClearRemotes);

  const char* headerkeys[] = {"Cookie"};
  server.collectHeaders(headerkeys, 1);

  server.begin();
}

// ================= LOOP =================
void loop() {

  server.handleClient();

  if (relayActive && millis() - relayStart >= RELAY_DURATION) {
    digitalWrite(RELAY_PIN, HIGH);
    relayActive = false;
  }

  if (rf.available()) {

    uint32_t code = rf.getReceivedValue();

    Serial.print("RF recebido: ");
    Serial.println(code);

    if (learningMode) {

      if (remoteCount < MAX_REMOTES) {
        remotes[remoteCount++] = code;
        saveRemotes();

        Serial.print("Controle cadastrado: ");
        Serial.println(code);
      }

      learningMode = false;

    } else {
      if (remoteAuthorized(code)) {

        Serial.println("Controle autorizado");

        if (!relayActive) {
          relayActive = true;
          relayStart = millis();
          digitalWrite(RELAY_PIN, LOW);
        }

      } else {
        Serial.println("Controle não cadastrado");
      }
    }
    rf.resetAvailable();
  }
}