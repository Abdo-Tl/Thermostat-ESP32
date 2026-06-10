/*
 * ============================================================
 *  Projet 1 – Thermostat Intelligent avec Programmation Horaire
 *  Auteur  : ABDELILAH TOUIL
 *  Carte   : ESP32 DevKit C v4
 *  Sim     : Wokwi (Wi-Fi : Wokwi-GUEST)
 *  Version : 1.0.0
 * ============================================================
 *
 *  Broches utilisées :
 *    GPIO4  → DS18B20 (OneWire)
 *    GPIO15 → DHT22
 *    GPIO26 → LED Rouge  (chauffage actif)
 *    GPIO27 → LED Bleue  (climatisation active)
 *    GPIO14 → Buzzer     (alerte température critique)
 *    GPIO21 → LCD SDA
 *    GPIO22 → LCD SCL
 *    GPIO13 → Bouton +   (augmenter consigne)
 *    GPIO12 → Bouton -   (diminuer consigne)
 *
 *  Blynk :
 *    V0 → Jauge Température DS18B20
 *    V1 → Jauge Humidité DHT22
 *    V2 → Slider Consigne
 *    V3 → LED État chauffage
 * ============================================================
 */

// ─── Bibliothèques ────────────────────────────────────────────
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"   // <-- remplace par ton Template ID
#define BLYNK_TEMPLATE_NAME "Thermostat"
#define BLYNK_AUTH_TOKEN    "TON_AUTH_TOKEN"  // <-- remplace par ton token

#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHTesp.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

// ─── Constantes de broches ────────────────────────────────────
const int PIN_DS18B20  = 4;
const int PIN_DHT22    = 15;
const int PIN_LED_R    = 26;
const int PIN_LED_B    = 27;
const int PIN_BUZZER   = 14;
const int PIN_BTN_PLUS = 13;
const int PIN_BTN_MINUS= 12;

// ─── Constantes métier ────────────────────────────────────────
const float HYSTERESIS       = 0.5;   // ±0.5 °C
const float TEMP_CRITIQUE_HT = 35.0;  // Alerte haute
const float TEMP_CRITIQUE_BAS= 5.0;   // Alerte basse
const float OFFSET_ECO       = -3.0;  // Mode éco nuit (-3 °C)

// Plage horaire mode éco simulée en millisecondes
// Principe : on simule une journée de 24 h = 86 400 000 ms
// Mode éco de 23 h à 7 h → [82 800 000 ms … 86 400 000 ms] ∪ [0 … 25 200 000 ms]
const unsigned long DUREE_JOURNEE_MS = 86400000UL;
const unsigned long ECO_DEBUT_MS     = 82800000UL; // 23 h * 3600 * 1000
const unsigned long ECO_FIN_MS       = 25200000UL; // 7  h * 3600 * 1000

// Intervalles de rafraîchissement (ms)
const unsigned long INTERVAL_LECTURE = 2000;
const unsigned long INTERVAL_LCD     = 1000;
const unsigned long INTERVAL_BLYNK   = 3000;
const unsigned long INTERVAL_BTN     = 200;

// ─── Wi-Fi ────────────────────────────────────────────────────
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ─── Objets globaux ───────────────────────────────────────────
OneWire           oneWire(PIN_DS18B20);
DallasTemperature dallas(&oneWire);
DHTesp            dht;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ─── Variables d'état ─────────────────────────────────────────
float   tempDS18B20   = 0.0;
float   tempDHT22     = 0.0;
float   humDHT22      = 0.0;
float   consigne      = 22.0;   // Consigne initiale
bool    chauffageON   = false;
bool    climON        = false;
bool    alerteON      = false;
bool    modeEco       = false;

// Timers logiciels (millis)
unsigned long tLecture = 0;
unsigned long tLCD     = 0;
unsigned long tBlynk   = 0;
unsigned long tBtn     = 0;

// Consommation énergétique (Partie E)
unsigned long tDebutChauffage = 0;
unsigned long dureeChaufTotal = 0;  // ms cumulées
const float   PUISSANCE_W     = 1000.0; // Puissance nominale simulée (W)

// ─── Prototypes ───────────────────────────────────────────────
void initMatériel();
void connecterWifi();
bool lireDS18B20();
bool lireDHT22();
void gererHysteresis();
bool estModeEco();
float consigneEffective();
void gererActionneurs();
void gererBoutons();
void afficherLCD();
void envoyerBlynk();
void gererAlertes();
void mettreAJourConso(bool chauffageEtat);

// ─── Callback Blynk : slider consigne (V2) ────────────────────
/**
 * @brief Reçoit la nouvelle consigne depuis le slider Blynk V2.
 * @param param Valeur envoyée par Blynk (float).
 */
BLYNK_WRITE(V2) {
  consigne = param.asFloat();
  Serial.printf("[Blynk] Nouvelle consigne reçue : %.1f °C\n", consigne);
}

// ════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("=== Thermostat Intelligent – Démarrage ===");

  initMatériel();
  connecterWifi();

  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS);
  Serial.println("[Blynk] Connecté.");
}

// ════════════════════════════════════════════════════════════════
//  LOOP  – jamais de delay() ici
// ════════════════════════════════════════════════════════════════
void loop() {
  Blynk.run();

  unsigned long maintenant = millis();

  // ── Lectures capteurs ──────────────────────────────────────
  if (maintenant - tLecture >= INTERVAL_LECTURE) {
    tLecture = maintenant;
    lireDS18B20();
    lireDHT22();
    modeEco = estModeEco();
    gererHysteresis();
    gererActionneurs();
    gererAlertes();
  }

  // ── Boutons ────────────────────────────────────────────────
  if (maintenant - tBtn >= INTERVAL_BTN) {
    tBtn = maintenant;
    gererBoutons();
  }

  // ── Affichage LCD ──────────────────────────────────────────
  if (maintenant - tLCD >= INTERVAL_LCD) {
    tLCD = maintenant;
    afficherLCD();
  }

  // ── Envoi Blynk ────────────────────────────────────────────
  if (maintenant - tBlynk >= INTERVAL_BLYNK) {
    tBlynk = maintenant;
    envoyerBlynk();
  }
}

// ════════════════════════════════════════════════════════════════
//  FONCTIONS
// ════════════════════════════════════════════════════════════════

/**
 * @brief Initialise les broches, le LCD, les capteurs.
 * @return void
 */
void initMatériel() {
  // Sorties
  pinMode(PIN_LED_R,    OUTPUT);
  pinMode(PIN_LED_B,    OUTPUT);
  pinMode(PIN_BUZZER,   OUTPUT);
  digitalWrite(PIN_LED_R,  LOW);
  digitalWrite(PIN_LED_B,  LOW);
  digitalWrite(PIN_BUZZER, LOW);

  // Entrées avec pull-up interne
  pinMode(PIN_BTN_PLUS,  INPUT_PULLUP);
  pinMode(PIN_BTN_MINUS, INPUT_PULLUP);

  // LCD I2C
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Thermostat v1.0");
  lcd.setCursor(0, 1);
  lcd.print("Initialisation..");

  // Capteurs
  dallas.begin();
  dht.setup(PIN_DHT22, DHTesp::DHT22);

  Serial.println("[Init] Matériel initialisé.");
}

/**
 * @brief Tente la connexion Wi-Fi ; affiche l'état sur le Serial.
 * @return void
 */
void connecterWifi() {
  Serial.printf("[Wi-Fi] Connexion à %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long debut = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - debut < 10000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[Wi-Fi] Connecté – IP : %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[Wi-Fi] ERREUR – Connexion échouée (mode hors-ligne).");
  }
}

/**
 * @brief Lit la température du DS18B20 via OneWire/DallasTemperature.
 * @return true si lecture valide, false si erreur capteur.
 */
bool lireDS18B20() {
  dallas.requestTemperatures();
  float t = dallas.getTempCByIndex(0);

  if (t == DEVICE_DISCONNECTED_C || t < -55.0 || t > 125.0) {
    Serial.println("[DS18B20] ERREUR – Capteur non répondu ou hors plage.");
    return false;
  }
  tempDS18B20 = t;
  Serial.printf("[DS18B20] T = %.2f °C\n", tempDS18B20);
  return true;
}

/**
 * @brief Lit la température et l'humidité du DHT22.
 * @return true si lecture valide, false si erreur capteur.
 */
bool lireDHT22() {
  TempAndHumidity th = dht.getTempAndHumidity();

  if (dht.getStatus() != DHTesp::ERROR_NONE) {
    Serial.printf("[DHT22] ERREUR – %s\n", dht.getStatusString());
    return false;
  }

  if (th.temperature < -40.0 || th.temperature > 80.0 ||
      th.humidity < 0.0      || th.humidity > 100.0) {
    Serial.println("[DHT22] ERREUR – Valeurs hors plage.");
    return false;
  }

  tempDHT22 = th.temperature;
  humDHT22  = th.humidity;
  Serial.printf("[DHT22] T = %.2f °C  H = %.1f %%\n", tempDHT22, humDHT22);
  return true;
}

/**
 * @brief Détecte si le système est en plage horaire éco (simulée via millis).
 *        Plage éco : 23 h → 7 h  (simulation : modulo DUREE_JOURNEE_MS).
 * @return true si mode éco actif.
 */
bool estModeEco() {
  unsigned long heure = millis() % DUREE_JOURNEE_MS;
  // Éco si heure >= 23h OU heure < 7h
  return (heure >= ECO_DEBUT_MS || heure < ECO_FIN_MS);
}

/**
 * @brief Retourne la consigne effective (avec offset éco si applicable).
 * @return float Consigne en °C.
 */
float consigneEffective() {
  return modeEco ? consigne + OFFSET_ECO : consigne;
}

/**
 * @brief Applique la logique d'hystérésis sur tempDS18B20 vs consigneEffective.
 *        Chauffage ON si T < consigne - 0.5
 *        Chauffage OFF si T > consigne + 0.5
 *        Clim    ON si T > consigne + 0.5  (même seuil, sens inverse)
 * @return void
 */
void gererHysteresis() {
  float c = consigneEffective();

  if (tempDS18B20 < c - HYSTERESIS) {
    chauffageON = true;
    climON      = false;
  } else if (tempDS18B20 > c + HYSTERESIS) {
    chauffageON = false;
    climON      = true;
  } else {
    // Dans la plage → on garde l'état précédent (cœur de l'hystérésis)
    climON = false; // pas de climatisation si dans la plage neutre
  }

  Serial.printf("[Hystérèse] Consigne=%.1f°C (éco=%s) | Chauf=%s | Clim=%s\n",
    c, modeEco ? "OUI" : "NON",
    chauffageON ? "ON" : "OFF",
    climON      ? "ON" : "OFF");
}

/**
 * @brief Pilote les actionneurs (LEDs, buzzer) selon l'état du thermostat.
 * @return void
 */
void gererActionneurs() {
  digitalWrite(PIN_LED_R, chauffageON ? HIGH : LOW);
  digitalWrite(PIN_LED_B, climON      ? HIGH : LOW);
  mettreAJourConso(chauffageON);
}

/**
 * @brief Déclenche le buzzer si température critique (< 5 °C ou > 35 °C).
 * @return void
 */
void gererAlertes() {
  bool critique = (tempDS18B20 < TEMP_CRITIQUE_BAS || tempDS18B20 > TEMP_CRITIQUE_HT);

  if (critique && !alerteON) {
    alerteON = true;
    digitalWrite(PIN_BUZZER, HIGH);
    Serial.printf("[ALERTE] Température critique : %.2f °C\n", tempDS18B20);

    // Notification Blynk
    if (Blynk.connected()) {
      Blynk.logEvent("alerte_temp",
        String("T critique : ") + tempDS18B20 + " °C");
    }
  } else if (!critique && alerteON) {
    alerteON = false;
    digitalWrite(PIN_BUZZER, LOW);
    Serial.println("[ALERTE] Retour normal – Buzzer OFF.");
  }
}

/**
 * @brief Gère les boutons poussoirs avec anti-rebond logiciel.
 *        Bouton + (GPIO13) : consigne += 0.5 °C
 *        Bouton - (GPIO12) : consigne -= 0.5 °C
 * @return void
 */
void gererBoutons() {
  static bool etatPlusPrec  = HIGH;
  static bool etatMoinsPrec = HIGH;

  bool etatPlus  = digitalRead(PIN_BTN_PLUS);
  bool etatMoins = digitalRead(PIN_BTN_MINUS);

  // Front descendant = appui (INPUT_PULLUP)
  if (etatPlus == LOW && etatPlusPrec == HIGH) {
    consigne += 0.5;
    Serial.printf("[Bouton+] Consigne → %.1f °C\n", consigne);
  }
  if (etatMoins == LOW && etatMoinsPrec == HIGH) {
    consigne -= 0.5;
    Serial.printf("[Bouton-] Consigne → %.1f °C\n", consigne);
  }

  etatPlusPrec  = etatPlus;
  etatMoinsPrec = etatMoins;
}

/**
 * @brief Met à jour l'affichage LCD 16x2.
 *        Ligne 0 : T° réelle + consigne
 *        Ligne 1 : Mode actif (chauffe/clim/ok) + éco
 * @return void
 */
void afficherLCD() {
  lcd.clear();

  // Ligne 0 : "T:23.4 C C:22.0"
  lcd.setCursor(0, 0);
  lcd.printf("T:%.1f C C:%.1f", tempDS18B20, consigneEffective());

  // Ligne 1 : état + mode éco
  lcd.setCursor(0, 1);
  if (alerteON) {
    lcd.print("!! ALERTE TEMP !!");
  } else if (chauffageON) {
    lcd.print("CHAUFFE ");
  } else if (climON) {
    lcd.print("CLIM    ");
  } else {
    lcd.print("OK      ");
  }

  if (modeEco) {
    lcd.setCursor(9, 1);
    lcd.print("[ECO]");
  }
}

/**
 * @brief Envoie les données vers les widgets Blynk (V0, V1, V3).
 * @return void
 */
void envoyerBlynk() {
  if (!Blynk.connected()) {
    Serial.println("[Blynk] Non connecté – envoi ignoré.");
    return;
  }

  Blynk.virtualWrite(V0, tempDS18B20);          // Jauge T° DS18B20
  Blynk.virtualWrite(V1, humDHT22);             // Jauge Humidité DHT22
  Blynk.virtualWrite(V3, chauffageON ? 1 : 0);  // LED état chauffage

  // Historique T° sur graphe (Partie E)
  // V4 utilisé pour le graphe historique 24h si configuré dans Blynk
  Blynk.virtualWrite(V4, tempDS18B20);

  // Consommation estimée (Partie E)
  float consoKWh = (dureeChaufTotal / 3600000.0) * (PUISSANCE_W / 1000.0);
  Blynk.virtualWrite(V5, consoKWh);

  Serial.printf("[Blynk] Envoyé → T=%.2f H=%.1f Chauf=%d Conso=%.4f kWh\n",
    tempDS18B20, humDHT22, chauffageON, consoKWh);
}

/**
 * @brief Calcule et accumule la durée de chauffage pour la consommation.
 * @param chauffageEtat État actuel du chauffage.
 * @return void
 */
void mettreAJourConso(bool chauffageEtat) {
  static bool etatPrecedent = false;
  unsigned long maintenant  = millis();

  if (chauffageEtat && !etatPrecedent) {
    // Chauffage vient de s'allumer
    tDebutChauffage = maintenant;
  } else if (!chauffageEtat && etatPrecedent) {
    // Chauffage vient de s'éteindre
    dureeChaufTotal += (maintenant - tDebutChauffage);
  } else if (chauffageEtat && etatPrecedent) {
    // Chauffage toujours ON : cumul en cours (optionnel pour affichage temps réel)
    // On ne cumule ici pour éviter la double comptabilisation
  }

  etatPrecedent = chauffageEtat;
}
