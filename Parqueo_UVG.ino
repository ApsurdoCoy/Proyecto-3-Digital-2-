/*************************************************************************************************
  Proyecto 3 - Centro de Control para Parqueo (Parqueo-matic)        IE3054 Electronica Digital 2
  ESP32: maestro I2C + servidor web

  - Lee por I2C el estado de las dos Nucleo (cada una maneja una fila de 4 parqueos).
  - Reenvia a cada Nucleo el estado completo de los 8 parqueos (para la GUI y el display).

**************************************************************************************************/

//************************************************************************************************
// Librerias
//************************************************************************************************
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <DNSServer.h>      // portal: cualquier direccion abre la pagina del parqueo
#include <ESPmDNS.h>        // nombre local: http://parqueo.local
#include "soc/soc.h"        
#include "soc/rtc_cntl_reg.h"

//************************************************************************************************
// Configuracion general
//************************************************************************************************

// ---- Modo WiFi ----
// 1 = Access Point: el ESP32 crea su propia red (no depende del WiFi del lugar)
// 0 = Estacion (STA): el ESP32 se conecta a una red existente (p. ej. hotspot del celular)
#define MODO_AP          1

// ---- Simulacion ----
// 1 = no usa I2C; los parqueos se cambian escribiendo 1..8 en el Monitor Serie
//     (sirve para probar la pagina web antes de tener lista la comunicacion I2C)
// 0 = funcionamiento normal con las dos Nucleo
#define SIMULACION       0

// ---- Red en modo AP ----
const char* ap_ssid     = "ParqueoUVG";     // se le agrega el final de la MAC -> nombre unico
                                               // (ej. "ParqueoUVG-3A7F"), ver Monitor Serie
const char* ap_password = "231020coy";          // minimo 8 caracteres
// La IP del ESP32 en modo AP es la de fabrica: 192.168.4.1

// ---- Red en modo STA ----
const char* sta_ssid     = "iPhone122";
const char* sta_password = "qwerty123";

// ---- I2C ----
#define I2C_SDA          21
#define I2C_SCL          22
#define I2C_FREQ         100000                // 100 kHz (Standard Mode, igual que las Nucleo)
#define ADDR_NUCLEO1     0x20                  // Nucleo de la pantalla  -> P1..P4
#define ADDR_NUCLEO2     0x21                  // Nucleo del display     -> P5..P8
#define PERIODO_I2C_MS   100                   // consulta a las Nucleo cada 100 ms
#define I2C_TIMEOUT_MS   10                    // espera maxima por transaccion I2C
#define REINTENTO_MS     1000                  // si una Nucleo no responde, reintenta cada 1 s

#define NUM_PARQUEOS     8

//************************************************************************************************
// Variables globales
//************************************************************************************************
WebServer server(80);
DNSServer dnsServer;                           // responde a cualquier nombre con la IP del ESP32

// Estado del parqueo: bit i = 1 -> P(i+1) ocupado
volatile uint8_t  mascaraOcupados = 0x00;
volatile uint32_t numCambios      = 0;         // aumenta en cada cambio (lo usa la pagina)
bool nucleo1OK = false;                        // la Nucleo respondio en la ultima consulta
bool nucleo2OK = false;

uint32_t tI2C = 0;
uint32_t tReintento1 = 0, tReintento2 = 0;     // para no bloquear el servidor si una Nucleo falla
uint32_t tDiag = 0;                            // diagnostico periodico por serial

//************************************************************************************************
// Prototipos
//************************************************************************************************
void    WiFi_Iniciar(void);
void    I2C_Actualizar(void);
bool    I2C_LeerFila(uint8_t direccion, uint8_t *fila);
bool    I2C_EnviarEstado(uint8_t direccion, uint8_t estado);
void    Simulacion_Leer(void);
void    I2C_Diagnostico(void);
void    Estado_Nuevo(uint8_t nueva);
uint8_t ContarLibres(uint8_t mascara);
void    handle_Raiz(void);
void    handle_Estado(void);
void    handle_NotFound(void);

//************************************************************************************************
// Pagina web (HTML + CSS + JavaScript). Se guarda en flash.
// La pagina pide /estado cada 400 ms y solo redibuja cuando el contador de cambios aumenta.
//************************************************************************************************
const char PAGINA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Parqueo-UVG</title>
<style>
  :root { --libre:#2ecc71; --ocupado:#e74c3c; --fondo:#eef1f4; --texto:#1e2329; }
  * { box-sizing:border-box; }
  body { margin:0; font-family:Helvetica, Arial, sans-serif; background:var(--fondo); color:var(--texto); }
  header { background:#1e2329; color:#fff; padding:14px 16px; }
  header h1 { margin:0; font-size:22px; }
  header p  { margin:4px 0 0; font-size:13px; opacity:.75; }
  main { max-width:760px; margin:0 auto; padding:16px; }
  .resumen { display:flex; gap:12px; margin-bottom:16px; }
  .tarjeta { flex:1; background:#fff; border-radius:10px; padding:12px; text-align:center;
             box-shadow:0 1px 3px rgba(0,0,0,.1); }
  .tarjeta .num { font-size:44px; font-weight:bold; }
  .tarjeta .lbl { font-size:13px; color:#666; }
  .lote { background:#555a62; border-radius:10px; padding:10px; }
  .fila { display:grid; grid-template-columns:repeat(4, 1fr); gap:8px; }
  .carril { height:34px; border-top:2px dashed #ddd; border-bottom:2px dashed #ddd; margin:8px 0; }
  .esp { height:110px; border-radius:6px; border-left:4px solid #f5be28; border-right:4px solid #f5be28;
         display:flex; flex-direction:column; align-items:center; justify-content:center;
         color:#fff; font-weight:bold; transition:background .3s; }
  .esp .id { font-size:20px; }
  .esp .st { font-size:12px; margin-top:4px; }
  .libre   { background:var(--libre); }
  .ocupado { background:var(--ocupado); }
  .nucleos { display:flex; gap:12px; font-size:13px; margin:12px 0; }
  .punto { display:inline-block; width:10px; height:10px; border-radius:50%; margin-right:4px; }
  h2 { font-size:16px; margin:18px 0 8px; }
  #log { list-style:none; padding:0; margin:0; background:#fff; border-radius:10px;
         max-height:220px; overflow-y:auto; box-shadow:0 1px 3px rgba(0,0,0,.1); }
  #log li { padding:8px 12px; border-bottom:1px solid #eee; font-size:14px; }
  #toast { position:fixed; left:50%; bottom:20px; transform:translateX(-50%); background:#1e2329;
           color:#fff; padding:12px 18px; border-radius:8px; opacity:0; transition:opacity .3s;
           pointer-events:none; max-width:90%; }
  #toast.ver { opacity:1; }
</style>
</head>
<body>
<header>
  <h1>Parqueo-UVG &#128664;</h1>
  <p>Monitoreo en tiempo real &middot; Nivel piloto (8 espacios)</p>
</header>
<main>
  <div class="resumen">
    <div class="tarjeta"><div class="num" id="libres">-</div><div class="lbl">Espacios libres</div></div>
    <div class="tarjeta"><div class="num" id="ocupados">-</div><div class="lbl">Ocupados</div></div>
  </div>

  <div class="lote">
    <div class="fila" id="fila1"></div>
    <div class="carril"></div>
    <div class="fila" id="fila2"></div>
  </div>

  <div class="nucleos">
    <span><span class="punto" id="n1"></span>Nucleo 1 (P1-P4)</span>
    <span><span class="punto" id="n2"></span>Nucleo 2 (P5-P8)</span>
    <span><span class="punto" id="web"></span>Servidor</span>
  </div>

  <h2>Notificaciones</h2>
  <ul id="log"><li>Esperando datos...</li></ul>
</main>
<div id="toast"></div>

<script>
  // ---- Crea los 8 espacios ----
  for (let i = 0; i < 8; i++) {
    const d = document.createElement('div');
    d.className = 'esp libre';
    d.id = 'p' + i;
    d.innerHTML = '<span class="id">P' + (i + 1) + '</span><span class="st">LIBRE</span>';
    document.getElementById(i < 4 ? 'fila1' : 'fila2').appendChild(d);
  }

  let ultimoCambio = -1;   // contador de cambios del ESP32
  let ultimaMascara = null;

  function hora() { return new Date().toLocaleTimeString(); }

  function notificar(texto) {
    const log = document.getElementById('log');
    if (log.firstChild && log.firstChild.textContent === 'Esperando datos...') log.innerHTML = '';
    const li = document.createElement('li');
    li.textContent = hora() + '  -  ' + texto;
    log.insertBefore(li, log.firstChild);
    const t = document.getElementById('toast');
    t.textContent = texto;
    t.classList.add('ver');
    clearTimeout(t.timer);
    t.timer = setTimeout(() => t.classList.remove('ver'), 2500);
  }

  function dibujar(e) {
    for (let i = 0; i < 8; i++) {
      const ocup = (e.ocupados >> i) & 1;
      const d = document.getElementById('p' + i);
      d.className = 'esp ' + (ocup ? 'ocupado' : 'libre');
      d.querySelector('.st').textContent = ocup ? 'OCUPADO' : 'LIBRE';
    }
    document.getElementById('libres').textContent   = e.libres;
    document.getElementById('ocupados').textContent = 8 - e.libres;
    document.getElementById('libres').style.color   = e.libres > 0 ? 'var(--libre)' : 'var(--ocupado)';
    document.getElementById('n1').style.background  = e.n1 ? 'var(--libre)' : 'var(--ocupado)';
    document.getElementById('n2').style.background  = e.n2 ? 'var(--libre)' : 'var(--ocupado)';
  }

  async function consultar() {
    try {
      const r = await fetch('/estado', { cache: 'no-store' });
      const e = await r.json();
      document.getElementById('web').style.background = 'var(--libre)';

      if (e.cambios !== ultimoCambio) {
        if (ultimaMascara !== null) {
          // Notificacion por cada espacio que cambio
          const dif = e.ocupados ^ ultimaMascara;
          for (let i = 0; i < 8; i++) {
            if ((dif >> i) & 1) {
              notificar('P' + (i + 1) + (((e.ocupados >> i) & 1) ? ' ocupado' : ' liberado') +
                        '  (libres: ' + e.libres + ')');
            }
          }
        } else {
          notificar('Conectado. Espacios libres: ' + e.libres);
        }
        ultimoCambio  = e.cambios;
        ultimaMascara = e.ocupados;
        dibujar(e);
      } else {
        dibujar(e);   // actualiza tambien el estado de conexion de las Nucleo
      }
    } catch (err) {
      document.getElementById('web').style.background = 'var(--ocupado)';
    }
    setTimeout(consultar, 400);
  }
  consultar();
</script>
</body>
</html>
)rawliteral";

//************************************************************************************************
// Configuracion
//************************************************************************************************
void setup() {
  // El pico de corriente al encender el WiFi hace caer el voltaje por un instante y el
  // detector de "brownout" reinicia el ESP32. Se desactiva para que el sistema arranque.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("Parqueo-UVG - ESP32 (I2C maestro + Web Server)");

#if !SIMULACION
  I2C_Diagnostico();                           // revisa lineas y busca las Nucleo
  Serial.println("I2C maestro iniciado (SDA=21, SCL=22)");
  Serial.println("Escriba 's' en el Monitor Serie para repetir el escaneo I2C");
#else
  Serial.println("MODO SIMULACION: escriba 1..8 en el Monitor Serie para cambiar un parqueo");
#endif

  WiFi_Iniciar();

  server.on("/", handle_Raiz);
  server.on("/estado", handle_Estado);
  server.on("/hotspot-detect.html", handle_Raiz);      // Apple (Mac / iPhone)
  server.on("/generate_204", handle_Raiz);             // Android
  server.on("/connecttest.txt", handle_Raiz);          // Windows
  server.onNotFound(handle_NotFound);
  server.begin();
  Serial.println("HTTP server started");
}

//************************************************************************************************
// Loop principal
//************************************************************************************************
void loop() {
#if MODO_AP
  dnsServer.processNextRequest();              // portal cautivo
#endif
  server.handleClient();                       // atiende a los navegadores conectados

#if SIMULACION
  Simulacion_Leer();
#else
  if (Serial.available() && (Serial.peek() == 's' || Serial.peek() == 'S')) {
    Serial.read();
    I2C_Diagnostico();                         // 's' = escanear de nuevo
  } else if (Serial.available()) {
    Serial.read();
  }
  if (millis() - tI2C >= PERIODO_I2C_MS) {     // consulta periodica a las Nucleo
    tI2C = millis();
    I2C_Actualizar();
  }
#endif

  // ---- Diagnostico cada 5 s: dispositivos conectados al WiFi y estado I2C ----
  if (millis() - tDiag >= 5000) {
    tDiag = millis();
#if MODO_AP
    Serial.printf("[diag] Dispositivos en la red: %d | Nucleo1: %s | Nucleo2: %s\n",
                  WiFi.softAPgetStationNum(), nucleo1OK ? "OK" : "sin respuesta",
                  nucleo2OK ? "OK" : "sin respuesta");
#else
    Serial.printf("[diag] WiFi: %s | Nucleo1: %s | Nucleo2: %s\n",
                  WiFi.status() == WL_CONNECTED ? "conectado" : "desconectado",
                  nucleo1OK ? "OK" : "sin respuesta", nucleo2OK ? "OK" : "sin respuesta");
#endif
  }
}

//************************************************************************************************
// WiFi: modo AP o modo estacion
//************************************************************************************************
void WiFi_Iniciar(void) {
#if MODO_AP
  WiFi.mode(WIFI_AP);
  // Nombre unico: "Parqueo-UVG-XXXX" con los ultimos 4 digitos de la MAC del ESP32
  uint8_t mac[6];
  WiFi.softAPmacAddress(mac);
  char ssidUnico[40];
  snprintf(ssidUnico, sizeof(ssidUnico), "%s-%02X%02X", ap_ssid, mac[4], mac[5]);
  WiFi.softAP(ssidUnico, ap_password);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);          // menos potencia = menos picos de corriente
  WiFi.setSleep(false);                        // conexion mas estable
  delay(100);
  dnsServer.start(53, "*", WiFi.softAPIP());   // cualquier nombre -> IP del ESP32
  MDNS.begin("parqueo");                       // http://parqueo.local
  Serial.print("Red WiFi creada: ");
  Serial.print(ssidUnico);
  Serial.print("   contrasena: ");
  Serial.println(ap_password);
  Serial.print("Pagina web: http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("   o   http://parqueo.local");
#else
  WiFi.mode(WIFI_STA);
  WiFi.begin(sta_ssid, sta_password);
  Serial.print("Conectando a ");
  Serial.println(sta_ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  MDNS.begin("parqueo");                       // http://parqueo.local
  Serial.print("Conectado. Pagina web: http://");
  Serial.print(WiFi.localIP());
  Serial.println("   o   http://parqueo.local");
#endif
}

//************************************************************************************************
// I2C: lee la fila de cada Nucleo y les envia el estado completo
//   Lectura  (1 byte): bits 0..3 = parqueos de esa fila (1 = ocupado)
//   Escritura(1 byte): mascara completa P1..P8 (bit0 = P1 ... bit7 = P8)
//************************************************************************************************
void I2C_Actualizar(void) {
  uint8_t fila1 = mascaraOcupados & 0x0F;          // si no responde, se conserva lo anterior
  uint8_t fila2 = (mascaraOcupados >> 4) & 0x0F;

  // Si una Nucleo no respondio, solo se reintenta cada REINTENTO_MS (asi el
  // servidor web sigue atendiendo rapido aunque una Nucleo este desconectada)
  if (nucleo1OK || millis() - tReintento1 >= REINTENTO_MS) {
    tReintento1 = millis();
    nucleo1OK = I2C_LeerFila(ADDR_NUCLEO1, &fila1);
  }
  if (nucleo2OK || millis() - tReintento2 >= REINTENTO_MS) {
    tReintento2 = millis();
    nucleo2OK = I2C_LeerFila(ADDR_NUCLEO2, &fila2);
  }

  uint8_t nueva = (fila1 & 0x0F) | ((fila2 & 0x0F) << 4);
  if (nueva != mascaraOcupados) {
    Estado_Nuevo(nueva);
  }

  // Cada Nucleo recibe el estado completo (GUI de la Nucleo 1 / total del display en la Nucleo 2)
  if (nucleo1OK) I2C_EnviarEstado(ADDR_NUCLEO1, mascaraOcupados);
  if (nucleo2OK) I2C_EnviarEstado(ADDR_NUCLEO2, mascaraOcupados);
}

bool I2C_LeerFila(uint8_t direccion, uint8_t *fila) {
  if (Wire.requestFrom((int)direccion, (int)1) != 1) {
    return false;                                  // la Nucleo no respondio
  }
  *fila = Wire.read() & 0x0F;
  return true;
}

bool I2C_EnviarEstado(uint8_t direccion, uint8_t estado) {
  Wire.beginTransmission(direccion);
  Wire.write(estado);
  return Wire.endTransmission() == 0;
}

//************************************************************************************************
// Diagnostico I2C: nivel de las lineas (pull-ups) + escaneo de direcciones
//************************************************************************************************
void I2C_Diagnostico(void) {
  Wire.end();
  pinMode(I2C_SDA, INPUT);
  pinMode(I2C_SCL, INPUT);
  delay(5);
  int sda = digitalRead(I2C_SDA);
  int scl = digitalRead(I2C_SCL);
  Serial.printf("[i2c] Lineas en reposo: SDA=%d  SCL=%d  %s\n", sda, scl,
                (sda && scl) ? "(OK, pull-ups presentes)"
                             : "(ERROR: deben ser 1 -> revisar pull-ups 3V3 / corto a GND)");

  Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);

  Serial.print("[i2c] Escaneando... ");
  uint8_t encontrados = 0;
  for (uint8_t dir = 0x08; dir < 0x78; dir++) {
    Wire.beginTransmission(dir);
    if (Wire.endTransmission() == 0) {
      Serial.printf("0x%02X ", dir);
      encontrados++;
    }
  }
  if (encontrados == 0) Serial.print("ningun dispositivo");
  Serial.println();
  Serial.println("[i2c] Esperado: 0x20 (Nucleo 1) y 0x21 (Nucleo 2)");
}

//************************************************************************************************
// Simulacion por Monitor Serie (SIMULACION = 1)
//************************************************************************************************
void Simulacion_Leer(void) {
  nucleo1OK = true;
  nucleo2OK = true;
  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '8') {
      Estado_Nuevo(mascaraOcupados ^ (1u << (c - '1')));
    }
  }
}

//************************************************************************************************
// Registro de un cambio de estado
//************************************************************************************************
void Estado_Nuevo(uint8_t nueva) {
  uint8_t dif = nueva ^ mascaraOcupados;
  mascaraOcupados = nueva;
  numCambios++;

  for (uint8_t i = 0; i < NUM_PARQUEOS; i++) {
    if (dif & (1u << i)) {
      Serial.printf("P%u %s  |  Libres: %u\n", i + 1,
                    (nueva & (1u << i)) ? "OCUPADO" : "LIBRE", ContarLibres(nueva));
    }
  }
}

uint8_t ContarLibres(uint8_t mascara) {
  uint8_t libres = 0;
  for (uint8_t i = 0; i < NUM_PARQUEOS; i++)
    if (!(mascara & (1u << i))) libres++;
  return libres;
}

//************************************************************************************************
// Handlers del servidor web
//************************************************************************************************
// Pagina principal
void handle_Raiz(void) {
  Serial.println("[web] Pagina solicitada por un navegador");
  server.send_P(200, "text/html", PAGINA_HTML);
}

// Estado en formato JSON (la pagina lo consulta cada 400 ms)
void handle_Estado(void) {
  char json[96];
  snprintf(json, sizeof(json),
           "{\"ocupados\":%u,\"libres\":%u,\"cambios\":%lu,\"n1\":%u,\"n2\":%u}",
           mascaraOcupados, ContarLibres(mascaraOcupados), (unsigned long)numCambios,
           nucleo1OK ? 1 : 0, nucleo2OK ? 1 : 0);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// Cualquier otra direccion se redirige a la pagina del parqueo
void handle_NotFound(void) {
#if MODO_AP
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
#else
  server.sendHeader("Location", "http://" + WiFi.localIP().toString() + "/", true);
#endif
  server.send(302, "text/plain", "");
}