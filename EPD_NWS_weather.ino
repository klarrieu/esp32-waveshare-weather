// HTTP/JSON stuff
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"
// EPD stuff
#include <PNGdec.h>
#include "DEV_Config.h"
#include "EPD.h"
#include "GUI_Paint.h"
#include <stdlib.h>


WiFiMulti wifiMulti;
HTTPClient http;
String forecast_endpoint;
String hourly_forecast_endpoint;
JsonDocument weekly_forecast;
JsonArray weekly_forecast_periods;

UBYTE* BlackImage;
/* you have to edit the startup_stm32fxxx.s file and set a big enough heap size */
UWORD Imagesize = ((EPD_7IN5_V2_WIDTH % 8 == 0) ? (EPD_7IN5_V2_WIDTH / 8) : (EPD_7IN5_V2_WIDTH / 8 + 1)) * EPD_7IN5_V2_HEIGHT;


void setup() {
  // start Serial stream
  Serial.begin(115200);
  Serial.println("Setup...");
  // connect to wifi
  waitForWifi();
  // init and clear display image
  DEV_Module_Init();
  EPD_7IN5_V2_Init();
  DEV_Delay_ms(500);
  if ((BlackImage = (UBYTE*)malloc(Imagesize)) == NULL) {
    Serial.println("Failed to apply for black memory...\r\n");
    while (1)
      ;
  }
  // clear image on display
  Paint_NewImage(BlackImage, EPD_7IN5_V2_WIDTH, EPD_7IN5_V2_HEIGHT, 0, WHITE);
  // get endpoints for weekly forecast
  getEndpoints();
}

void waitForWifi() {
  wifiMulti.addAP(SSID, WIFI_PASSWORD);
  int i = 0;
  Serial.println("Waiting for wifi...");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.printf("%i s\n", i);
    delay(1000);
    i++;
  }
}

JsonDocument getJson(String endpoint) {
  // send GET request to endpoint and parse response into JsonDocument
  JsonDocument doc;
  http.begin(endpoint);
  int httpCode = http.GET();
  if (httpCode > 0) {
    // HTTP header has been send and Server response header has been handled
    Serial.printf("[HTTP] GET... code: %d\n", httpCode);
    // file found at server
    if (httpCode == HTTP_CODE_OK) {
      deserializeJson(doc, http.getStream());
    }
  }
  http.end();
  return doc;
}

void getEndpoints() {
  // get forecast and hourly forecast endpoints
  // wait for WiFi connection
  Serial.println("Getting endpoints...");
  if ((wifiMulti.run() == WL_CONNECTED)) {
    JsonDocument doc = getJson("https://api.weather.gov/points/39.24,-120.02");
    JsonObject properties = doc["properties"];
    forecast_endpoint = properties["forecast"].as<String>();
    hourly_forecast_endpoint = properties["forecastGridData"].as<String>();
    Serial.println(forecast_endpoint);
    Serial.println(hourly_forecast_endpoint);
  }
}

// NWS icon stuff
// Declare PNG decoder
PNG png;
// Buffer to hold the downloaded PNG data
uint8_t* pngDataBuffer = nullptr;
int pngDataSize = 0;
// Image dimensions
int imgWidth = 134;   // width, large=134
int imgHeight = 134;  // height

struct ImageState {
  int y;  // Row counter
  int imgWidth;
  int imgHeight;
  uint8_t* grayscaleImage;  // Temporary grayscale buffer
};
ImageState imageState;

void Draw_Icon(String endpoint, UWORD xStart, UWORD yStart) {
  // Set up image state
  imageState.y = 0;
  imageState.imgWidth = imgWidth;
  imageState.imgHeight = imgHeight;
  imageState.grayscaleImage = nullptr;
  // width of image in bytes
  int w_byte = (imgWidth % 8) ? (imgWidth / 8) + 1 : imgWidth / 8;
  // get image using http
  http.begin(endpoint);
  int httpCode = http.GET();
  if (httpCode > 0) {
    // HTTP header has been sent and Server response header has been handled
    Serial.printf("[HTTP] GET... code: %d\n", httpCode);
    // file found at server
    if (httpCode == HTTP_CODE_OK) {
      // Get the content length (size of the PNG file)
      pngDataSize = http.getSize();
      // Allocate memory for the PNG data
      pngDataBuffer = (uint8_t*)malloc(pngDataSize);
      // Read the data into the buffer
      WiFiClient* stream = http.getStreamPtr();
      int bytesRead = 0;
      while (http.connected() && bytesRead < pngDataSize) {
        bytesRead += stream->readBytes(pngDataBuffer + bytesRead, pngDataSize - bytesRead);
      }
      Serial.printf("Downloaded PNG file, size: %d bytes\n", bytesRead);
    }
  }
  http.end();
  Serial.println("Reading PNG data butter...");
  // Decode the pngdata
  int16_t rc = png.openRAM(pngDataBuffer, pngDataSize, pngDrawRGB);
  if (rc != PNG_SUCCESS) {
    Serial.printf("Error opening PNG from memory: %d\n", rc);
    return;
  }
  Serial.println("Decoding...");
  // Decode image to grayscale
  png.decode(NULL, 0);
  png.close();
  Serial.print("Before freeing pngbuffer:");
  Serial.println(ESP.getFreeHeap());
  free(pngDataBuffer);  // Free the PNG buffer after copying to grayscaleImage
  Serial.println("Dithering...");
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
  uint8_t* ditheredImage = new uint8_t[w_byte * imgHeight];
  // initialize as white image
  memset(ditheredImage, 0xFF, w_byte * imgHeight);
  Serial.println("Allocated ditheredImage");
  applyFloydSteinbergDithering(imageState.grayscaleImage, ditheredImage, imgWidth, imgHeight);
  Serial.print("Before freeing grayscaleImage:");
  Serial.println(ESP.getFreeHeap());
  delete[] imageState.grayscaleImage;
  Serial.println("Painting icon...");
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
  Paint_DrawImage(ditheredImage, xStart, yStart, imgWidth, imgHeight);
  delete[] ditheredImage;
}

void pngDrawRGB(PNGDRAW* pDraw) {
  // Allocate grayscale buffer if it's the first pass
  if (imageState.grayscaleImage == nullptr) {
    imageState.grayscaleImage = new uint8_t[imageState.imgWidth * imageState.imgHeight];
  }
  // Process each pixel, converting to grayscale
  for (int x = 0; x < pDraw->iWidth; x++) {
    int r = pDraw->pPixels[x * 3];
    int g = pDraw->pPixels[x * 3 + 1];
    int b = pDraw->pPixels[x * 3 + 2];
    // Convert RGB to grayscale using luminance formula
    uint8_t grayscale = clip((r * 0.299) + (g * 0.587) + (b * 0.114));
    // Store grayscale value in the temporary buffer
    imageState.grayscaleImage[imageState.y * pDraw->iWidth + x] = grayscale;
  }
  // Increment the row counter
  imageState.y++;
}

void applyFloydSteinbergDithering(uint8_t* grayscaleImage, uint8_t* packedImage, int width, int height) {
  // width of image in bytes
  int w_byte = (width % 8) ? (width / 8) + 1 : width / 8;
  // Iterate through each pixel and apply dithering while packing into 1-bit format
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      // Get the grayscale pixel value
      int index = y * width + x;
      uint8_t oldPixel = grayscaleImage[index];
      uint8_t newPixel = (oldPixel > 127) ? 255 : 0;
      int error = oldPixel - newPixel;
      // Pack the pixel into the packedImage buffer
      int byteIndex = (y * w_byte) + (x / 8);
      int bitPosition = 7 - (x % 8);  // Position of the bit in the byte
      if (newPixel == 255) {
        packedImage[byteIndex] |= (1 << bitPosition);  // Set the bit to 1
      } else {
        packedImage[byteIndex] &= ~(1 << bitPosition);  // Set the bit to 0
      }
      // Distribute the error using Floyd-Steinberg weights
      // propagate error by clipping within range
      if (x + 1 < width) {
        grayscaleImage[index + 1] = clip(grayscaleImage[index + 1] + error * 7 / 16);
      }
      if (y + 1 < height) {
        if (x > 0) {
          grayscaleImage[index + width - 1] = clip(grayscaleImage[index + width - 1] + error * 3 / 16);
        }
        grayscaleImage[index + width] = clip(grayscaleImage[index + width] + error * 5 / 16);
        if (x + 1 < width) {
          grayscaleImage[index + width + 1] = clip(grayscaleImage[index + width + 1] + error * 1 / 16);
        }
      }
    }
  }
}

uint8_t clip(float n){
  // clip value to 0-255 range
  return std::max((float)0, std::min(n, (float)255));
}

void DrawString_centered(UWORD Xstart, UWORD Ystart, const char* pString,
                         sFONT* Font, UWORD Color_Foreground, UWORD Color_Background) {
  // draw string with top center at Xstart, Ystart
  // TODO: also have max chars for wrapping to next line
  //calculate x shift
  const int width = Font->Width;
  Xstart -= int(0.5 * width * strlen(pString));
  Paint_DrawString_EN(Xstart, Ystart, pString, Font, Color_Foreground, Color_Background);
}

// TODO: make wrapping smart by splitting words if we are going to run out of space
void DrawString_wrap(UWORD Xstart, UWORD Ystart, const char* pString,
                         sFONT* Font, UWORD Color_Foreground, UWORD Color_Background) {
  // split words by spaces
  // char* word = strtok(pString, " ");
  // Serial.println(word);
}

void DrawWeather() {
  // Start by clearing image
  Paint_SelectImage(BlackImage);
  Paint_Clear(WHITE);
  // populate image with weather forecast bits
  for (int i = 0; i < 4; i++) {
    Serial.printf("loop %d\n", i);
    JsonObject period = weekly_forecast_periods[i];
    // get period name and short forecast
    const char* name = period["name"].as<const char*>();
    const char* shortForecast = period["shortForecast"];
    // get url for icon
    String icon = period["icon"];
    icon.replace("medium", "large");
    // get high/low temperature and unit
    int temperature = period["temperature"];
    const char* temperatureUnit = period["temperatureUnit"];
    const char* detailedForecast = period["detailedForecast"];
    char tempStr[10];
    sprintf(tempStr, "%i %s", temperature, temperatureUnit);
    // get detailed forecast
    char forecastStr[400];
    sprintf(forecastStr, "%s: %s", name, detailedForecast);
    // print stuff out
    Serial.println(name);
    Serial.println(shortForecast);
    Serial.println(icon);
    Serial.println(tempStr);
    Serial.println(forecastStr);
    // draw to EPD
    // TODO: wrap text without splitting words
    // TODO: make shortForecast shorter or wrap if too long
    DrawString_centered(180 * i + 90, 0, name, &Font12x23, WHITE, BLACK);
    DrawString_centered(180 * i + 90, 155, shortForecast, &Font12x23, WHITE, BLACK);
    DrawString_centered(180 * i + 90, 185, tempStr, &Font12x23, WHITE, BLACK);
    Paint_DrawString_EN(0, 220 + 50 * i, forecastStr, &Font12x23, WHITE, BLACK);
    Draw_Icon(icon, 180 * i + 23, 20);
  }
  // display image for 15 s
  Serial.println("Displaying...");
  EPD_7IN5_V2_Display(BlackImage);
}


void wakeUp(){
  Serial.println("Awake!");
  DEV_Module_Init();
  EPD_7IN5_V2_Init();
  waitForWifi();
}

void loop() {
  Serial.println("New loop");
  weekly_forecast = getJson(forecast_endpoint);
  weekly_forecast_periods = weekly_forecast["properties"]["periods"];
  DrawWeather();
  Serial.println("Sleeping 15 min...");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(900000000);
  esp_deep_sleep_start();
  wakeUp();
}
