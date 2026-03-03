//version 0.2

#include <WiFi.h>
#include <WebServer.h>
#include <Arduino.h>
#include <Crypto.h>
#include <SHA256.h>
#include <esp_random.h>
#include <time.h>

const char* ssid = "WIFI_NAME";
const char* password = "WIFI_PASS";

const int RELAY_PIN = 5;
WebServer server(80);

// Usuário e senha (hash SHA256 da senha "1234")
const String USERNAME = "admin";
const String PASSWORD_HASH = "03ac674216f3e15c761ee1a5e255f067953623c8f9660a1b2c7b2d2a1c1ee9a5";

// Sessão
String sessionToken = "";
time_t sessionExpiry = 0; // agora usando hora real
const time_t SESSION_DURATION = 7UL * 24UL * 60UL * 60UL; // 1 semana em segundos

// Pulso do relé
bool relayActive = false;
unsigned long relayStart = 0;
const unsigned long RELAY_DURATION = 500; // 0.5 seg

// Configuração NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600; // GMT-3
const int daylightOffset_sec = 0;
unsigned long lastNtpSync = 0;
const unsigned long NTP_SYNC_INTERVAL = 12UL * 60UL * 60UL * 1000UL; // 12h
bool timeSynced = false;   // indica se a hora real já foi sincronizada
const int MAX_NTP_RETRIES = 5;

// HTML login
const char* loginPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Login</title>
<style>
body { display:flex; justify-content:center; align-items:center; height:100vh; background:#2a2a2a; font-family:sans-serif; margin:0; }
form { display:flex; flex-direction:column; background:#333; padding:40px; border-radius:15px; box-shadow: 0 10px 25px rgba(0,0,0,0.6); width:90%; max-width:400px; }
input { margin:10px 0; padding:15px; font-size:1.5em; border-radius:10px; border:none; outline:none; }
button { margin-top:20px; padding:20px; font-size:1.8em; border-radius:50px; background: linear-gradient(145deg,#FFD700,#FFC300); color:#222; border:none; cursor:pointer; box-shadow: 0 10px 20px rgba(0,0,0,0.5), inset 0 -6px 15px rgba(0,0,0,0.3), inset 0 6px 10px rgba(255,255,255,0.2); transition: all 0.2s ease; }
button:hover { transform: scale(1.05); box-shadow: 0 12px 25px rgba(0,0,0,0.6), inset 0 -6px 15px rgba(0,0,0,0.3), inset 0 6px 10px rgba(255,255,255,0.25); }
button:active { transform: scale(0.95); box-shadow: 0 5px 10px rgba(0,0,0,0.4), inset 0 -3px 8px rgba(0,0,0,0.3), inset 0 3px 6px rgba(255,255,255,0.2); }
</style>
</head>
<body>
<form method="POST" action="/login">
<input type="text" name="user" placeholder="Usuário" required>
<input type="password" name="pass" placeholder="Senha" required>
<button type="submit">Entrar</button>
</form>
</body>
</html>
)rawliteral";

// HTML relé
String relayPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Relé</title>
<style>
body { display:flex; justify-content:center; align-items:center; height:100vh; background:#2a2a2a; margin:0; font-family:sans-serif; }
button { width:400px; height:400px; border-radius:50%; border:none; background: linear-gradient(145deg,#FFD700,#FFC300); color:#222; font-size:3.5em; cursor:pointer; box-shadow: 0 10px 25px rgba(0,0,0,0.6), inset 0 -6px 15px rgba(0,0,0,0.3), inset 0 6px 10px rgba(255,255,255,0.2); transition: all 0.2s ease; text-shadow:0 5px 5px rgba(0,0,0,0.4); }
button:hover { transform: scale(1.05); box-shadow: 0 12px 30px rgba(0,0,0,0.7), inset 0 -6px 15px rgba(0,0,0,0.3), inset 0 6px 10px rgba(255,255,255,0.25); }
button:active { transform: scale(0.95); box-shadow: 0 5px 15px rgba(0,0,0,0.5), inset 0 -3px 8px rgba(0,0,0,0.3), inset 0 3px 6px rgba(255,255,255,0.2); }
</style>
</head>
<body>
<button id="activateBtn">Ativar</button>
<script>
document.getElementById("activateBtn").addEventListener("click", () => {
  fetch('/pulse').then(r=>r.text()).then(alert);
});
</script>
</body>
</html>
)rawliteral";
}

// Função SHA-256
String sha256(const String &input) {
  byte hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0); // 0 = SHA-256
  mbedtls_sha256_update_ret(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  char hashStr[65];
  for (int i = 0; i < 32; i++) sprintf(hashStr + i*2, "%02x", hash[i]);
  hashStr[64] = 0;

  return String(hashStr);
}


// Configura hora via NTP com retry
void syncTime() {
  int retries = 0;
  while (!timeSynced && retries < MAX_NTP_RETRIES) {
    Serial.println("Tentando sincronizar NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      timeSynced = true;
      lastNtpSync = millis();
      Serial.println("Hora NTP sincronizada com sucesso!");
      break;
    } else {
      retries++;
      Serial.println("Falha ao sincronizar hora. Tentando novamente...");
      yield();
      delay(2000); // espera 2s antes da próxima tentativa
    }
  }

  if (!timeSynced) {
    Serial.println("Não foi possível sincronizar NTP. Usando fallback com millis().");
    // Aqui podemos usar millis() + offset como fallback temporário
  }
}

// Função para pegar a hora atual com fallback
time_t getCurrentTime() {
  if (timeSynced) {
    return time(NULL);
  } else {
    // Fallback: usamos millis() desde boot como "tempo relativo"
    static time_t fallbackBase = 0;
    if (fallbackBase == 0) fallbackBase = millis() / 1000; // marca o início do fallback
    return fallbackBase + millis() / 1000;
  }
}

// Verifica cookie usando hora real
bool isLoggedIn() {
  if (!server.hasHeader("Cookie")) return false;
  String cookie = server.header("Cookie");
  int start = cookie.indexOf("session=");
  if (start == -1) return false;
  start += 8;
  int end = cookie.indexOf(";", start);
  String token = (end == -1) ? cookie.substring(start) : cookie.substring(start, end);

  time_t now = getCurrentTime();
  return (token == sessionToken && now < sessionExpiry);
}

// Handlers
void handleLogin() {
  if (isLoggedIn()) {
    server.sendHeader("Location","/relay");
    server.send(303);
    return;
  }

  if (server.method() == HTTP_POST) {
    String user = server.arg("user");
    String pass = server.arg("pass");
    if (user==USERNAME && sha256(pass)==PASSWORD_HASH) {
      sessionToken = String((uint32_t)esp_random(), HEX) + String((uint32_t)esp_random(), HEX);
      sessionExpiry = getCurrentTime() + SESSION_DURATION;
      server.sendHeader("Set-Cookie", "session="+sessionToken+"; Max-Age=604800; HttpOnly");
      server.sendHeader("Location","/relay");
      server.send(303);
      return;
    } else {
      server.send(200,"text/html","<h1>Senha incorreta</h1><a href='/'>Voltar</a>");
      return;
    }
  }
  server.send(200,"text/html",loginPage);
}

void handleRelay() {
  if (!isLoggedIn()) {
    server.sendHeader("Location","/");
    server.send(303);
    return;
  }
  server.send(200,"text/html",relayPage());
}

void handlePulse() {
  if (!isLoggedIn()) {
    server.send(403,"text/plain","Não autorizado");
    return;
  }
  if(!relayActive){
    relayActive = true;
    relayStart = millis();
    digitalWrite(RELAY_PIN, LOW);
  }
  server.send(200,"text/plain","Relé acionado!");
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.begin(115200);

  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  syncTime(); // sincroniza hora ao iniciar

  server.on("/", handleLogin);
  server.on("/login", handleLogin);
  server.on("/relay", handleRelay);
  server.on("/pulse", handlePulse);
  server.begin();
}

void loop() {
  server.handleClient();

  // Relé não bloqueante
  if(relayActive && millis() - relayStart >= RELAY_DURATION){
    digitalWrite(RELAY_PIN, HIGH);
    relayActive = false;
  }

  // Resincroniza NTP a cada NTP_SYNC_INTERVAL
  if(millis() - lastNtpSync > NTP_SYNC_INTERVAL){
    syncTime();
  }
}