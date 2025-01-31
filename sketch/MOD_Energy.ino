// This firmware has been developed to be used on LilyGO-T-A7670E/G/SA R2 platforms in
// combination with a Sensirion SCD30 sensor. The device is responsible for detecting
// data related to temperature, relative humidity, and CO2 concentration at regular
// intervals, as well as transmitting this data to an external server, via the
// integrated SIMCOM A7670 module. The code can be customized by modifying the device ID,
// the APN parameters of the mobile network operator, the duration of deep sleep between
// measurements, the server address, and the string to be sent via a GET request.

// This code is written and maintained by Giancarlo Furcieri @ MIRPALab.
// It is provided "as is", without any express or implied warranty of any kind,
// including but not limited to the warranties of merchantability, fitness for
// a particular purpose, and non-infringement. In no event shall the author be
// liable for any claim, damages, or other liability, whether in an action of
// contract, tort, or otherwise, arising from, out of, or in connection with
// the code or the use or other dealings in the code.
// The following code is licensed under the GNU General Public License v3.0 (GPLv3).
// You may freely use, modify, and distribute this code under the terms of the GPLv3,
// which can be found at: https://www.gnu.org/licenses/gpl-3.0.html
// By using this code, you agree to comply with the terms of the GPLv3.

// Board model
#define LILYGO_T_A7670

// Select radio modem
#define TINY_GSM_MODEM_A7670

// Set RX buffer to 1KB
#define TINY_GSM_RX_BUFFER 1024

// I2C pins
#define I2C_SDA                             (21)
#define I2C_SCL                             (22)
#define I2C_CLKRATE_50K                     (50000)

// LilyGO T-A7670G Pinout
#define MODEM_BAUDRATE                      (115200)
#define MODEM_DTR_PIN                       (25)
#define MODEM_TX_PIN                        (26)
#define MODEM_RX_PIN                        (27)
// The modem boot pin needs to follow the startup sequence.
#define BOARD_PWRKEY_PIN                    (4)
#define BOARD_ADC_PIN                       (35)
// The modem power switch must be set to HIGH for the modem to supply power.
#define BOARD_POWERON_PIN                   (12)
#define MODEM_RING_PIN                      (33)
#define MODEM_RESET_PIN                     (5)
#define BOARD_MISO_PIN                      (2)
#define BOARD_MOSI_PIN                      (15)
#define BOARD_SCK_PIN                       (14)
#define BOARD_SD_CS_PIN                     (13)
#define BOARD_BAT_ADC_PIN                   (35)
#define MODEM_RESET_LEVEL                   HIGH

#define TINY_GSM_USE_GPRS true
#define TINY_GSM_USE_WIFI false

// Set serial for modem debug console (to the Serial Monitor, default speed 115200)
#define SerialMon Serial
#define SerialAT Serial1

#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <Wire.h>
#include <SensirionI2cScd30.h>

// Define the serial console for modem debug prints, if needed
#define TINY_GSM_DEBUG SerialMon

// Set device ID
String deviceID = "00";

// Set APN Details / GPRS credentials
const char apn[] = "mobile.vodafone.it";
const char gprsUser[] = "";
const char gprsPass[] = "";

// Define a conversion from microseconds to seconds
uint64_t us_to_s = 1000000;

// Set deep sleep timer to 3600 seconds in order to have a measure each hour
uint64_t time_to_sleep = 3600;

// Set sensor error message
static int16_t geterr;
static int16_t measerror;
static char errorMessage[128];

// Save our counter to RTCRAM
RTC_DATA_ATTR int count = 0;

// Save boolean value of a GET request result
RTC_DATA_ATTR bool attemptGET;

// Define a struct that will hold all measurements done in 24 hours
typedef struct
{
  float temperature;
  float humidity;
  float co2Concentration;
} sensorReadings;

// Set number of readings to hold in memory
#define maxReadings 24

// Declare an array "Readings" of type "sensorReadings" with size "maxReadings" 
RTC_DATA_ATTR sensorReadings Readings[maxReadings];

void setup()
{
  Serial.begin(115200);

// Set clock frequency to 10MHz in order to save energy
  setCpuFrequencyMhz(10);
  Serial.println("Waking up");
  Serial.print("Clock speed set at ");
  Serial.print(getCpuFrequencyMhz());
  Serial.println(" MHz");

// Keep the GSM modem turned off while taking measurements
  TinyGsm modem(SerialAT);
  modem.poweroff();

// Initialize sensor
  SensirionI2cScd30 sensor;
  while (!Serial)
  {
    delay(1000);
  }
  Wire.begin();
  Wire.setClock(I2C_CLKRATE_50K);
  sensor.begin(Wire, SCD30_I2C_ADDR_61);
  sensor.startPeriodicMeasurement(0);
  float temperature = 0.0;
  float humidity = 0.0;
  float co2Concentration = 0.0;

// Measurements fail-safe: if, for any reasons, the sensor doesn't take a measurement, it tries again up to 5 times
  int attemptData = 0;
  do
  {
    measerror = sensor.blockingReadMeasurementData(co2Concentration, temperature, humidity);
    if (measerror != NO_ERROR)
    {
      Serial.print("Error trying to execute blockingReadMeasurementData(): ");
      errorToString(measerror, errorMessage, sizeof errorMessage);
      Serial.println(errorMessage);
      delay(2000);
      attemptData++;
    }
  }
  while (measerror != NO_ERROR && attemptData < 5);

// Assign measurements in an array
  Readings[count].temperature = temperature;
  Readings[count].humidity = humidity;
  Readings[count].co2Concentration = co2Concentration;
  count++;
  delay(2000);
    sensor.stopPeriodicMeasurement();

// Print number of counter on serial monitor and wake up with the main timer (3600s)
  if (count < 25)
  {    
    Serial.println("Measure #"+String(count));
    esp_sleep_enable_timer_wakeup(time_to_sleep * us_to_s);
  }
  else
  {
    Serial.println("Starting data trasmission routine...");

// When count >= 25, the device took 24 measures and it sends the response to serial monitor.
// The device wakes up for the 25th time, sends 24 measures, takes a 25th measure but immediately restarts in order to clear RTCRAM
// Explanation of ">=": a GET request fail-safe has been implemented so if a GET fails, ESP32 will go to deep sleep.
// After "soft reset" the counter will increment; nonetheless, with a value >= 25, ESP32 must send measurement data to the server.

// Server details
    String server = "modenergy.ew.r.appspot.com";
    String resource = "/writeday?id="+String(deviceID)
    +"&t_00="+String(Readings[0].temperature)+"&h_00="+String(Readings[0].humidity)+"&c_00="+String(Readings[0].co2Concentration)
    +"&t_01="+String(Readings[1].temperature)+"&h_01="+String(Readings[1].humidity)+"&c_01="+String(Readings[1].co2Concentration)
    +"&t_02="+String(Readings[2].temperature)+"&h_02="+String(Readings[2].humidity)+"&c_02="+String(Readings[2].co2Concentration)
    +"&t_03="+String(Readings[3].temperature)+"&h_03="+String(Readings[3].humidity)+"&c_03="+String(Readings[3].co2Concentration)
    +"&t_04="+String(Readings[4].temperature)+"&h_04="+String(Readings[4].humidity)+"&c_04="+String(Readings[4].co2Concentration)
    +"&t_05="+String(Readings[5].temperature)+"&h_05="+String(Readings[5].humidity)+"&c_05="+String(Readings[5].co2Concentration)
    +"&t_06="+String(Readings[6].temperature)+"&h_06="+String(Readings[6].humidity)+"&c_06="+String(Readings[6].co2Concentration)
    +"&t_07="+String(Readings[7].temperature)+"&h_07="+String(Readings[7].humidity)+"&c_07="+String(Readings[7].co2Concentration)
    +"&t_08="+String(Readings[8].temperature)+"&h_08="+String(Readings[8].humidity)+"&c_08="+String(Readings[8].co2Concentration)
    +"&t_09="+String(Readings[9].temperature)+"&h_09="+String(Readings[9].humidity)+"&c_09="+String(Readings[9].co2Concentration)
    +"&t_10="+String(Readings[10].temperature)+"&h_10="+String(Readings[10].humidity)+"&c_10="+String(Readings[10].co2Concentration)
    +"&t_11="+String(Readings[11].temperature)+"&h_11="+String(Readings[11].humidity)+"&c_11="+String(Readings[11].co2Concentration)
    +"&t_12="+String(Readings[12].temperature)+"&h_12="+String(Readings[12].humidity)+"&c_12="+String(Readings[12].co2Concentration)
    +"&t_13="+String(Readings[13].temperature)+"&h_13="+String(Readings[13].humidity)+"&c_13="+String(Readings[13].co2Concentration)
    +"&t_14="+String(Readings[14].temperature)+"&h_14="+String(Readings[14].humidity)+"&c_14="+String(Readings[14].co2Concentration)
    +"&t_15="+String(Readings[15].temperature)+"&h_15="+String(Readings[15].humidity)+"&c_15="+String(Readings[15].co2Concentration)
    +"&t_16="+String(Readings[16].temperature)+"&h_16="+String(Readings[16].humidity)+"&c_16="+String(Readings[16].co2Concentration)
    +"&t_17="+String(Readings[17].temperature)+"&h_17="+String(Readings[17].humidity)+"&c_17="+String(Readings[17].co2Concentration)
    +"&t_18="+String(Readings[18].temperature)+"&h_18="+String(Readings[18].humidity)+"&c_18="+String(Readings[18].co2Concentration)
    +"&t_19="+String(Readings[19].temperature)+"&h_19="+String(Readings[19].humidity)+"&c_19="+String(Readings[19].co2Concentration)
    +"&t_20="+String(Readings[20].temperature)+"&h_20="+String(Readings[20].humidity)+"&c_20="+String(Readings[20].co2Concentration)
    +"&t_21="+String(Readings[21].temperature)+"&h_21="+String(Readings[21].humidity)+"&c_21="+String(Readings[21].co2Concentration)
    +"&t_22="+String(Readings[22].temperature)+"&h_22="+String(Readings[22].humidity)+"&c_22="+String(Readings[22].co2Concentration)
    +"&t_23="+String(Readings[23].temperature)+"&h_23="+String(Readings[23].humidity)+"&c_23="+String(Readings[23].co2Concentration);

// GSM MODULE PARAMETERS AND FUNCTIONS BLOCK
    TinyGsm modem(SerialAT);
    TinyGsmClient client(modem);
    HttpClient http(client, server);

// Set Serial Monitor baud rate
    SerialMon.begin(115200);
    delay(10);

// Turn on DC boost to power on the modem
    #ifdef BOARD_POWERON_PIN
    pinMode(BOARD_POWERON_PIN, OUTPUT);
    digitalWrite(BOARD_POWERON_PIN, HIGH);
    #endif

// Set modem reset
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
  
// Turn on modem
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(1000);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

    SerialMon.println("Wait...");

// Set GSM module baud rate and pins
    SerialAT.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    Serial.println("Starting modem...");
    delay(3000);
    SerialMon.println(F("Initializing modem..."));
    if (!modem.init())
    {
      SerialMon.println(F("Failed to restart modem, delaying 10s and retrying"));
      return;
    }

    #if TINY_GSM_USE_GPRS
// GPRS connection parameters are usually set after network registration
    SerialMon.print(F("Connecting to "));
    SerialMon.print(apn);

// Network fail-safe: if, for any reasons, the radio module doesn't connect to a GSM network, it tries again up to 5 times
    int attemptGPRS = 0;
    do
    {
      if (!modem.gprsConnect(apn, gprsUser, gprsPass))
      {
        SerialMon.println(": Fail");
        delay(5000);
        attemptGPRS++;
        SerialMon.print("Retry... ");
      }
      else
      {
        SerialMon.println(": Success");
      }
    }
    while (!modem.gprsConnect(apn, gprsUser, gprsPass) && attemptGPRS < 5);

    if (modem.isGprsConnected())
    {
      SerialMon.println("GPRS connected");
    }
    #endif

// The following code is needed for HTTPS
    http.connectionKeepAlive();

// Connection timeout set to 90 seconds
    http.setTimeout(9000);
    SerialMon.print(F("Performing HTTP GET request... "));

// GET fail-safe: if, for any reasons, a GET request fails, it tries again until it succeeds
    do
    {
      geterr = http.get(resource);
      if (geterr != 0)
      {
        attemptGET = false;
        SerialMon.println(F("GET request failed"));
        esp_sleep_enable_timer_wakeup(10 * us_to_s);
        esp_deep_sleep_start();
      }
      else
      {
        attemptGET = true;
        SerialMon.println(F("GET request succeeded"));
      }
    }
    while (geterr != 0 && attemptGET == false);

    int status = http.responseStatusCode();
    SerialMon.print(F("Response status code: "));
    SerialMon.println(status);
    if (!status)
    {
      delay(1000);
      return;
    }

// Shutdown
    http.stop();
    SerialMon.println(F("Server disconnected"));

    #if TINY_GSM_USE_GPRS
    modem.gprsDisconnect();
    SerialMon.println(F("GPRS disconnected"));
    modem.poweroff();
    #endif

// Restart ESP32 in order to clear RTCRAM
    Serial.println("Restarting ESP32 in order to clear RTCRAM...");
    delay(1000);
    esp_restart();
  }

// Go back to deep sleep
  Serial.println("Going to sleep");
  esp_deep_sleep_start();
}

void loop()
{
// No code here  
}
