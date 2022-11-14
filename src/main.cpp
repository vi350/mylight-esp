#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>

#include <GyverEncoder.h>
#include "GyverButton.h"
#include <TM74HC595Display.h>

// заранее просчитанные значения гамма коррекции
// диапазон 0-100 для перевода сразу из значения яркости с дисплея
const unsigned int CRTgammaPGM[101] PROGMEM = {
    0,
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16, 17, 19, 21, 24, 27,
    29, 32, 36, 39, 42, 46, 50, 54, 59, 63,
    68, 73, 78, 83, 89, 94, 100, 107, 113, 120,
    127, 134, 141, 149, 157, 165, 173, 182, 190, 199,
    209, 218, 228, 238, 249, 259, 270, 281, 293, 305,
    317, 329, 342, 354, 368, 381, 395, 409, 423, 438,
    452, 468, 483, 499, 515, 532, 548, 565, 583, 600,
    618, 637, 655, 674, 694, 713, 733, 753, 774, 795,
    816, 838, 860, 882, 905, 928, 951, 975, 999, 1023,
};

// захардкоженные параметры локалки
#define ssid "yourssid"
#define password "yourpass"
#define sn "1" // serial number, по задумке — уникальный номер устройства в локальной сети

// переменные
const byte led = 15;
unsigned long change_brightness_timer, send_brightness_timer, disp_timer; // таймеры
bool state = true; // состояние, для включения и выключения без потери установленного значения яркости
bool setted_by_enc = false; // флаг для определения источника чем была изменена яркость, энкодером или из веба
byte brightness = 0; // фактическая яркость в моменте
byte goal_brightness = 100; // яркость, к которой стремимся прямо сейчас
byte setted_brightness = 100; // установленное значение, отделена от предыдущей для плавного включения и выключения через state
byte last_setted_brightness = 100; // для отправки яркости на сервер только при изменении
byte mode = 0; // задел на будущее, режимы отображения

// объекты
GButton butt1(5);
Encoder enc1(13, 12, ENC_NO_BUTTON, TYPE1);
TM74HC595Display disp(14, 2, 4);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiClient wificlient;
HTTPClient http;

unsigned int getBrightCRT(byte val) { // применения гамма коррекции к значению яркости
  return pgm_read_word(&(CRTgammaPGM[val]));
}
void change_brightness() { // плавно стремимся к целевой яркости
  if (goal_brightness != brightness & millis() - change_brightness_timer > 20) {
    change_brightness_timer = millis();
    if (goal_brightness > brightness) {
      brightness += 1;
    } else {
      brightness -= 1;
    }
  }
}
void send_changed_brightness() { // если яркость изменилась через энкодер, отослать значение на сервер
  if (setted_by_enc & millis() - send_brightness_timer > 200 & setted_brightness != last_setted_brightness) {
    send_brightness_timer = millis();
    last_setted_brightness = setted_brightness;
    // отсылка на сервер
  }
}
void toggle_via_goal() { // через state определяем к чему стремится яркость — к установленному значению или к 0
  if (state) goal_brightness = setted_brightness;
  else goal_brightness = 0;
}
void enc_logic() { // обработка движений энкодера и нажатий кнопки
  if (butt1.isHolded()) { // изменение режима через удержание кнопки
    mode++;
    if (mode > 0) {
      mode = 0;
    }
  } else if (butt1.isClick()) {
    switch (mode) {
      case 0: // по нажатию меняем state и извещаем сервер об изменении
        state = !state;
        // отсылка на сервер
        http.end();
        break;
    }
  }

  switch (mode) { // выводим на дисплей значение в зависимости от режима
    case 0:
      disp.clear();
      disp.digit4(setted_brightness);
      break;
  }

  if (enc1.isRight()) {
    switch (mode) {
      case 0: // единичный поворот вправо -> изменение на +1 за одно значение поворота
        if (setted_brightness < 100) {
          setted_brightness += 1;
          setted_by_enc = true;
          disp.clear();
          disp.digit4(setted_brightness);
        }
        break;
    }
  } else if (enc1.isLeft()) {
    switch (mode) {
      case 0: // единичный поворот влево -> изменение на -1 за одно значение поворота
        if (setted_brightness > 0) {
          setted_brightness -= 1;
          setted_by_enc = true;
          disp.clear();
          disp.digit4(setted_brightness);
        }
        break;
    }
  } else if (enc1.isFastR()) {
    switch (mode) {
      case 0: // быстрый поворот вправо -> изменение на +5 за одно значение поворота
        if (setted_brightness < 100) {
          setted_brightness += 5;
          setted_brightness = min((int)setted_brightness, 100);
          setted_by_enc = true;
          disp.clear();
          disp.digit4(setted_brightness);
        }
        break;
    }
  } else if (enc1.isFastL()) {
    switch (mode) {
      case 0: // быстрый поворот влево -> изменение на -5 за одно значение поворота
        if (setted_brightness > 0) {
          setted_brightness -= 5;
          setted_brightness = max((int)setted_brightness, 0);
          setted_by_enc = true;
          disp.clear();
          disp.digit4(setted_brightness);
        }
        break;
    }
  }
}

// обработчики команд с сервера
String on() {
  state = true;
  return String(state);
}
String off() {
  state = false;
  return String(state);
}
String toggle() {
  state = !state;
  return String(state);
}
String status() {
  return String(state);
}
String set_brightness() {
  if (server.arg("brightness") != "") {
    int br = server.arg("brightness").toInt(); // временная переменная для приведения полученных данных к интервалу 0..100
    constrain(br, 0, 100);
    setted_brightness = (byte)br;
    setted_by_enc = false;
  }
  return String(setted_brightness);
}

void setup() {
  pinMode(led, OUTPUT);
  Serial.begin(9600);

  butt1.setDebounce(10);
  butt1.setTimeout(400); // таймаут для различия удержания и клика
  butt1.setType(LOW_PULL);
  enc1.setFastTimeout(100); // таймаут для различия быстрого и медленного вращения
  disp.digit4(brightness);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  if (digitalRead(5) == HIGH) {
    delay(1000);
    if (digitalRead(5) == HIGH){
      httpUpdater.setup(&server);
      server.begin();
      while (true) server.handleClient();
    }
  }

  server.begin();

  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection failed");
  } else {
    Serial.println(WiFi.localIP());
  }

  // хендлеры команд с сервера
  server.on("/toggle", [](){
      server.send(200, "text/plain", toggle());
  });
  server.on("/on", [](){
      server.send(200, "text/plain", on());
  });
  server.on("/off", [](){
      server.send(200, "text/plain", off());
  });
  server.on("/status", [](){
      server.send(200, "text/plain", status());
  });
  server.on("/brightness", [](){
      server.send(200, "text/plain", set_brightness());
  });
  server.on("/sn", [](){
      server.send(200, "text/plain", sn);
  });
  server.onNotFound([](){
      server.send(404, "text/plain", "Not Found");
  });
}

void loop() {
  change_brightness();
  toggle_via_goal();
  enc_logic();
  send_changed_brightness();

  // обновляем кнопку, энкодер, дисплей
  butt1.tick();
  enc1.tick();
  if (micros() - disp_timer > 300) {       // таймер динамической индикации
    disp.timerIsr();
    disp_timer = micros();
  }
  analogWrite(led, getBrightCRT(brightness));
  server.handleClient();
}
