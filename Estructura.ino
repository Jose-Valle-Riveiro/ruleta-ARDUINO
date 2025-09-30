#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Stepper.h>

//Configuración LCD
#define LCD_ADDR 0x27     
#define LCD_COLS 16
#define LCD_ROWS 2

// Pines
#define PIN_RC522_SS 10
#define PIN_RC522_RST 9
#define PIN_LED  6
#define PIN_BUZZER 7
#define PIN_BUTTON 8

// Stepper 28BYJ-48
#define STEPS_PER_REV 2048

Stepper stepperMotor(STEPS_PER_REV, 2, 4, 3, 5);
#define STEPPER_RPM 10         //  (5–15)

// Juego
#define MAX_PLAYERS 8     
#define HOLD_WIN_MS 3500       // tiempo mostrando ganador en LCD

// modo de juego:
#define MODO_CONSTANTE 0
#define MODO_REDUCCION 1
#define GAME_MODE MODO_CONSTANTE  

// Botón
#define BTN_DEBOUNCE_MS 50
#define BTN_LONG_PRESS_MS 1500

// LED parpadeo
#define BLINK_INTERVAL_MS 300

MFRC522 rfid(PIN_RC522_SS, PIN_RC522_RST);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

enum State {
  IDLE,         // A la espera: botón corto -> REGISTRO
  REGISTRO,     // Leyendo UIDs; botón corto -> LISTO
  LISTO         // Botón corto -> JUGAR
};

State estado = IDLE;

struct TagUID {
  uint8_t bytes[10];
  uint8_t length;
  bool activo;
};

TagUID jugadores[MAX_PLAYERS];

int numJugadores = 0;   // N actual (registrados)
long currentPos = 0;    // posición relativa del stepper en pasos

unsigned long lastButtonChange = 0;
bool lastButtonLevel = HIGH; // INPUT_PULLUP: reposo = HIGH
bool buttonPressed = false;
unsigned long buttonPressTime = 0;

unsigned long lastBlink = 0;
bool ledOn = false;

void lcdCenterPrint(uint8_t row, const String &msg) {
  int startCol = max(0, (LCD_COLS - (int)msg.length()) / 2);
  lcd.setCursor(startCol, row);
  lcd.print(msg);
}

String uidToHex(const TagUID &t) {
  String s;
  for (byte i = 0; i < t.length; i++) {
    if (t.bytes[i] < 0x10) s += "0";
    s += String(t.bytes[i], HEX);
  }
  s.toUpperCase();
  return s;
}

bool sameUID(const TagUID &a, const TagUID &b) {
  if (a.length != b.length) return false;
  for (byte i = 0; i < a.length; i++) {
    if (a.bytes[i] != b.bytes[i]) return false;
  }
  return true;
}

int findUIDIndex(const TagUID &t) {
  for (int i = 0; i < numJugadores; i++) {
    if (sameUID(jugadores[i], t)) return i;
  }
  return -1;
}

void addPlayer(const TagUID &t) {
  if (numJugadores >= MAX_PLAYERS) return;
  jugadores[numJugadores] = t;
  jugadores[numJugadores].activo = true;
  numJugadores++;
}

void clearPlayers() {
  numJugadores = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    jugadores[i].length = 0;
    jugadores[i].activo = false;
  }
}

int countActivos() {
  if (GAME_MODE == MODO_CONSTANTE) return numJugadores;
  int c = 0;
  for (int i = 0; i < numJugadores; i++) if (jugadores[i].activo) c++;
  return c;
}

// Elegir un índice de jugador activo [0..numJugadores-1]; si reducción, salta inactivos
int pickRandomJugador() {
  int activos = countActivos();
  if (activos <= 0) return -1;
  int target = random(activos); // 0..activos-1

  if (GAME_MODE == MODO_CONSTANTE) {
    return target;
  } else {
    // Mapear 'target' a índice real activo
    for (int i = 0, k = 0; i < numJugadores; i++) {
      if (jugadores[i].activo) {
        if (k == target) return i;
        k++;
      }
    }
    return -1; // no debería pasar
  }
}

void beep(unsigned int freq, unsigned long ms) {
  tone(PIN_BUZZER, freq);
  delay(ms);
  noTone(PIN_BUZZER);
}

void playWinnerJingle() {
  beep(523, 120);
  delay(30);
  beep(659, 120);
  delay(30);
  beep(784, 160);
  delay(50);
  beep(1046, 250);
}

// =============================== BOTÓN =================================
void readButton() {
  bool level = digitalRead(PIN_BUTTON);
  unsigned long now = millis();

  if (level != lastButtonLevel && (now - lastButtonChange) > BTN_DEBOUNCE_MS) {
    lastButtonChange = now;
    lastButtonLevel = level;

    if (level == LOW) { // pulsado
      buttonPressed = true;
      buttonPressTime = now;
    } else {
      // liberado
      if (buttonPressed) {
        unsigned long held = now - buttonPressTime;
        buttonPressed = false;

        if (held >= BTN_LONG_PRESS_MS) {
          // Pulsación larga -> RESET
          estado = IDLE;
          clearPlayers();
          noTone(PIN_BUZZER);
          digitalWrite(PIN_LED, LOW);
          lcd.clear();
          lcdCenterPrint(0, "Reiniciado");
          lcdCenterPrint(1, "Listo para reg.");
          delay(1000);
        } else {
          // Pulsación corta -> acción según estado
          if (estado == IDLE) {
            estado = REGISTRO;
            lcd.clear();
            lcdCenterPrint(0, "REGISTRO");
            lcd.setCursor(0, 1);
            lcd.print("Pase tarjeta...");
            rfid.PCD_AntennaOn();
          } else if (estado == REGISTRO) {
            if (numJugadores >= 2) {
              estado = LISTO;
              lcd.clear();
              lcdCenterPrint(0, "LISTO PARA JUGAR");
              lcd.setCursor(0, 1);
              lcd.print("N = ");
              lcd.print(numJugadores);
              rfid.PCD_AntennaOff();
              lcd.print(GAME_MODE == MODO_CONSTANTE ? " Const" : " Red");
              digitalWrite(PIN_LED, LOW); // LED fijo solo al jugar
            } else {
              lcd.clear();
              lcdCenterPrint(0, "Necesitas >=2");
              lcdCenterPrint(1, "jugadores");
              delay(1200);
              lcd.clear();
              lcdCenterPrint(0, "REGISTRO");
              lcd.setCursor(0, 1);
              lcd.print("Pase tarjeta...");
            }
          } else if (estado == LISTO) {
            // JUGAR
            jugarRonda();
          }
        }
      }
    }
  }
}

// =============================== LED ===================================
void updateLED() {
  if (estado == REGISTRO) {
    unsigned long now = millis();
    if (now - lastBlink >= BLINK_INTERVAL_MS) {
      lastBlink = now;
      ledOn = !ledOn;
      digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
    }
  } else if (estado == LISTO) {
    // En LISTO LED apagado; se enciende fijo al girar
    // (gestiona jugarRonda()).
  } else {
    digitalWrite(PIN_LED, LOW);
  }
}

// =============================== RFID ==================================
bool readCard(TagUID &out) {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return false;

  out.length = rfid.uid.size;
  if (out.length > 10) out.length = 10;
  for (byte i = 0; i < out.length; i++) out.bytes[i] = rfid.uid.uidByte[i];

  // parar lectura de la tarjeta actual
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return true;
}

// ============================== STEPPER =================================
void stepRelative(long steps) {
  // Mantener rastro de posición (mod STEPS_PER_REV)
  stepperMotor.step(steps);
  currentPos = (currentPos + steps) % STEPS_PER_REV;
  if (currentPos < 0) currentPos += STEPS_PER_REV;
}

// Gira al menos 1 vuelta y aterriza en el segmento del ganador
void spinToWinner(int idxGanador) {
  int participantes = countActivos();
  if (participantes <= 0) return;

  // tamaño de cada segmento
  long seg = STEPS_PER_REV / participantes;

  // calcular índice de segmento efectivo del 'idxGanador' entre activos
  int segIndex = 0;
  if (GAME_MODE == MODO_CONSTANTE) {
    segIndex = idxGanador;
  } else {
    // mapear a orden de activos compactados
    for (int i = 0, k = 0; i < numJugadores; i++) {
      if (jugadores[i].activo) {
        if (i == idxGanador) { segIndex = k; break; }
        k++;
      }
    }
  }

  // offset objetivo dentro de la vuelta
  long baseTarget = seg * segIndex;

  // aleatorio dentro del segmento (evitar bordes)
  long jitter = random(seg / 4, (seg * 3) / 4);

  // objetivo absoluto respecto a 0..STEPS_PER_REV-1
  long targetInRev = (baseTarget + jitter) % STEPS_PER_REV;

  // Paso 1: una vuelta completa para generar suspenso jsdkj
  tone(PIN_BUZZER, 200);         // sonido continuo mientras gira
  digitalWrite(PIN_LED, HIGH);   // LED fijo en juego
  stepRelative(STEPS_PER_REV);

  // Paso 2: ir hasta el objetivo de la 2a vuelta desde la posición actual
  long delta = targetInRev - currentPos;  // relativo
  if (delta < 0) delta += STEPS_PER_REV;

  stepRelative(delta);
  noTone(PIN_BUZZER);            // llegó
  // LED se mantiene encendido un momento para enfatizar
}

// =============================== JUEGO ==================================
void jugarRonda() {
  if (countActivos() < 2) {
    lcd.clear();
    lcdCenterPrint(0, "No hay suficientes");
    lcdCenterPrint(1, "jugadores");
    delay(1200);
    return;
  }

  lcd.clear();
  lcdCenterPrint(0, "Girando...");
  lcdCenterPrint(1, "Suerte!");

  int idx = pickRandomJugador();
  if (idx < 0) return;

  // Gira y aterriza
  spinToWinner(idx);

  // Mostrar ganador
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ganador: J");
  // Número (1..N) según orden de registro
  lcd.print(idx + 1);
  lcd.setCursor(0, 1);
  lcd.print("UID ");
  lcd.print(uidToHex(jugadores[idx]));

  playWinnerJingle();

  // Modo reducción: desactivar al ganador
  if (GAME_MODE == MODO_REDUCCION) {
    jugadores[idx].activo = false;
  }

  delay(HOLD_WIN_MS);

  // Volver a LISTO (si ya no hay jugadores suficientes, avisar)
  if (countActivos() < 2) {
    lcd.clear();
    lcdCenterPrint(0, "Fin de la partida");
    lcdCenterPrint(1, "Reinicia (long btn)");
  } else {
    lcd.clear();
    lcdCenterPrint(0, "LISTO PARA JUGAR");
    lcd.setCursor(0, 1);
    lcd.print("N = ");
    lcd.print(countActivos());
    lcd.print(GAME_MODE == MODO_CONSTANTE ? " Const" : " Red");
  }
  digitalWrite(PIN_LED, LOW); // apagar LED al finalizar la jugada
}

// =============================== SETUP ==================================
void setup() {
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  digitalWrite(PIN_LED, LOW);
  noTone(PIN_BUZZER);

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdCenterPrint(0, "Ruleta RFID");
  lcdCenterPrint(1, "Btn: Reg/Jugar");
  delay(1200);

  // Stepper
  stepperMotor.setSpeed(STEPPER_RPM);

  // RFID
  SPI.begin();
  rfid.PCD_Init();

  // Aleatoriedad
  randomSeed(analogRead(A0));

  // Estado inicial
  estado = IDLE;
  clearPlayers();
  lcd.clear();
  lcdCenterPrint(0, "Listo");
  lcdCenterPrint(1, "Btn: Registro");
}

// ================================ LOOP ==================================
void loop() {
  // Lectura de botón con debounce y detección de corto/largo
  readButton();

  // LED según estado
  updateLED();

  // Registro de tarjetas
  if (estado == REGISTRO) {
    TagUID t;
    if (readCard(t)) {
      if (numJugadores >= MAX_PLAYERS) {
        lcd.clear();
        lcdCenterPrint(0, "Lista llena (10)");
        delay(1000);
        lcd.clear();
        lcdCenterPrint(0, "Pulsa boton");
        lcdCenterPrint(1, "para terminar");
      } else {
        if (findUIDIndex(t) >= 0) {
          lcd.clear();
          lcdCenterPrint(0, "Tarjeta repetida");
          lcd.setCursor(0, 1);
          lcd.print(uidToHex(t));
          delay(1000);
          lcd.clear();
          lcdCenterPrint(0, "REGISTRO");
          lcd.setCursor(0, 1);
          lcd.print("Pase otra...");
        } else {
          addPlayer(t);
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Registrado J");
          lcd.print(numJugadores); 
          lcd.setCursor(0, 1);
          lcd.print(uidToHex(t));
          delay(900);
          lcd.clear();
          lcdCenterPrint(0, "REGISTRO");
          lcd.setCursor(0, 1);
          lcd.print("Total: ");
          lcd.print(numJugadores);
        }
      }
    }
  }
}