#include <Arduino.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <cstring>

#include <og3/blink_led.h>
#include <og3/constants.h>
#include <og3/din.h>
#include <og3/ha_app.h>
#include <og3/html_table.h>
#include <og3/oled_wifi_info.h>
#include <og3/shtc3.h>
#include <og3/units.h>
#include <og3/variable.h>

#define VERSION "0.8.0"

namespace og3 {

static const char kManufacturer[] = "Chris Lee";
static const char kModel[] = "Boiler";
static const char kSoftware[] = "Boiler v" VERSION;

#if defined(LOG_UDP) && defined(LOG_UDP_ADDRESS)
constexpr App::LogType kLogType = App::LogType::kUdp;
#else
// constexpr App::LogType kLogType = App::LogType::kNone;  // kSerial
constexpr App::LogType kLogType = App::LogType::kSerial;
#endif

HAApp s_app(HAApp::Options(kManufacturer, kModel,
                           WifiApp::Options()
                               .withSoftwareName(kSoftware)
                               .withDefaultDeviceName("boiler")
#if defined(LOG_UDP) && defined(LOG_UDP_ADDRESS)
                               .withUdpLogHost(IPAddress(LOG_UDP_ADDRESS))

#endif
                               .withOta(OtaManager::Options(OTA_PASSWORD))
                               .withApp(App::Options().withLogType(kLogType))));

// Hardware config
constexpr uint8_t kBoilerPin = 23;
constexpr uint8_t kRedLed = 18;
constexpr uint8_t kYellowLed = 19;
constexpr uint8_t kBlueLed = 20;

// Names
static const char kTemperature[] = "temperature";
static const char kHumidity[] = "humidity";

// Global variable for html, so asyncwebserver can send data in the background (single client)
String s_html;

// Delay between updates of the OLED.
constexpr unsigned kOledSwitchMsec = 5000;
OledDisplayRing s_oled(&s_app.module_system(), kSoftware, kOledSwitchMsec, Oled::kTenPt);

// This module tracks the state of the water detector using a digital input pin.
class WaterCheck : public Module {
 public:
  WaterCheck(uint8_t pin, HAApp* app_, VariableGroup& vg);

  void read() { m_din.read();
  }
  bool haveWater() const { return m_din.isHigh(); }

 private:
  HAApp* const m_app;
  HADependencies m_dependencies;
  VariableGroup& m_vg;
  DIn m_din;
};

constexpr unsigned kCfgSet = VariableBase::Flags::kConfig | VariableBase::Flags::kSettable;

WaterCheck::WaterCheck(uint8_t pin, HAApp* app_, VariableGroup& vg)
    : Module("boiler", &app_->module_system()),
      m_app(app_),
      m_vg(vg),
      m_din("boiler", &m_app->module_system(), pin, "boiler has water", m_vg, true/*publish*/,
	    true/*invert*/) {
  setDependencies(&m_dependencies);
  add_init_fn([this]() {
    if (m_dependencies.ok()) {
      m_dependencies.ha_discovery()->addDiscoveryCallback(
          [this](HADiscovery* had, JsonDocument* json) {
            return had->addBinarySensor(json, m_din.isHighVar(),
                                        ha::device_class::binary_sensor::kMoisture);
          });
      s_oled.addDisplayFn([this]() {
        if (!haveWater()) {
          s_oled.display("Fill boiler tank.");
        }
      });
    }
  });
}

class Monitor : public Module {
 public:
  Monitor(HAApp* app)
      : Module("monitor", &app->module_system()),
        m_app(app),
        // Every minute, read sensors and send readings via MQTT, starting in 10 seconds.
        m_mqtt_scheduler(
            10 * kMsecInSec, kMsecInMin, [this]() { sendMqtt(); }, &app->tasks()),
        m_vg("room"),
        m_water(kBoilerPin, app, m_vg),
        m_wifi_oled(&app->tasks()),
        m_ylw_blink("ylw_blink", kYellowLed, app, 500),
        m_blu_blink("blu_blink", kBlueLed, app, 500),
        m_shtc3(kTemperature, kHumidity, &app->module_system(), "temperature", m_vg) {
    add_init_fn([this]() {
      s_oled.addDisplayFn([this]() {
        char text[80];
        snprintf(text, sizeof(text), "%s %.1fC %.1fRH", m_water.haveWater() ? "OK" : "EMPTY!",
                 m_shtc3.temperature(), m_shtc3.humidity());
        s_oled.display(text);
	log().log(text);
      });
      m_app->config().read_config(m_vg);
    });
  }
  const VariableGroup& vg() const { return m_vg; }

  void readSensors() {
    m_shtc3.read();
    m_water.read();
  }

 private:
  Logger& log() { return m_app->log(); }
  void sendMqtt() {
    readSensors();
    m_app->mqttSend(m_vg);
  }

  HAApp* const m_app;
  // Send configuration every 5 minutes.
  PeriodicTaskScheduler m_mqtt_scheduler;
  VariableGroup m_vg;
  WaterCheck m_water;
  OledWifiInfo m_wifi_oled;
  BlinkLed m_ylw_blink;
  BlinkLed m_blu_blink;
  Shtc3 m_shtc3;
};

Monitor s_monitor(&s_app);

WebButton s_button_wifi_config = s_app.createWifiConfigButton();
WebButton s_button_mqtt_config = s_app.createMqttConfigButton();
WebButton s_button_app_status = s_app.createAppStatusButton();
WebButton s_button_restart = s_app.createRestartButton();

void handleWebRoot(AsyncWebServerRequest* request) {
  s_monitor.readSensors();
  s_html.clear();
  html::writeTableInto(&s_html, s_monitor.vg());
  html::writeTableInto(&s_html, s_app.wifi_manager().variables());
  html::writeTableInto(&s_html, s_app.mqtt_manager().variables());
  s_button_mqtt_config.add_button(&s_html);
  s_button_app_status.add_button(&s_html);
  s_button_restart.add_button(&s_html);
  sendWrappedHTML(request, s_app.board_cname(), kSoftware, s_html.c_str());
}

}  // namespace og3

////////////////////////////////////////////////////////////////////////////////

void setup() {
  og3::s_app.web_server().on("/", og3::handleWebRoot);
  og3::s_app.setup();
}

void loop() { og3::s_app.loop(); }
