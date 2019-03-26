#include "mlxcamera.h"
#include "interpolation.h"

#include "MLX90640_API.h"
#include "MLX90640_I2C_Driver.h"

paramsMLX90640 mlx90640;
#define TA_SHIFT 8 //Default shift for MLX90640 in open air

#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>

//#define DEBUG_INTERPOLATION

MLXCamera::MLXCamera(TFT_eSPI& _tft)
 : tft(_tft)
{
#ifdef DEBUG_INTERPOLATION
   for (int i = 0; i < 768; i++)
    rawPixels[i] = ((i + i / 32) % 2) == 0 ? 20.f : 30.f;
#endif
}

bool MLXCamera::init()
{
  // Connect thermal sensor.
  Wire.begin();
  Wire.setClock(400000); // Increase I2C clock speed to 400kHz

  if (!isConnected())
  {
    Serial.println("MLX90640 not detected at default I2C address. Please check wiring.");
    return false;
  }
    
  Serial.println("MLX90640 online!");
    
  // Get device parameters - We only have to do this once
  int status;
  uint16_t eeMLX90640[832];
  status = MLX90640_DumpEE(MLX90640_address, eeMLX90640);
  if (status != 0)
  {
    Serial.println("Failed to load system parameters");
    return false;
  }
    
  status = MLX90640_ExtractParameters(eeMLX90640, &mlx90640);
  if (status != 0)
  {
    Serial.println("Parameter extraction failed");
    return false;
  }

  MLX90640_SetChessMode(MLX90640_address);
  status = MLX90640_SetRefreshRate(MLX90640_address, 0x05); // Set rate to 8Hz effective - Works at 800kHz
  if (status != 0)
  {
    Serial.println("SetRefreshRate failed");
    return false;
  }
  
  Serial.printf("RefreshRate: %.1f Hz\n", getRefreshRateInHz());
  Serial.printf("Resolution: %d-bit\n", getResolutionInBit());
  if (isInterleaved())
    Serial.println("Mode: Interleaved");
  else
    Serial.println("Mode: Chess");

  
  // Once EEPROM has been read at 400kHz we can increase
  Wire.setClock(800000);

  SPI.begin();
  SPI.setFrequency(80000000L);  

  return true;
}

bool MLXCamera::isConnected() const
{
  Wire.beginTransmission((uint8_t)MLX90640_address);
  return Wire.endTransmission() == 0; //Sensor did not ACK
}

float MLXCamera::getRefreshRateInHz() const
{
   int rate = MLX90640_GetRefreshRate(MLX90640_address);
   switch(rate) {
    case 0: return 0.5f;
    case 1: return 1.f;
    case 2: return 2.f;
    case 3: return 4.f;
    case 4: return 8.f;
    case 5: return 16.f;
    case 6: return 32.f;
    case 7: return 64.f;
    default : return 0.f;
   }
}

int MLXCamera::getResolutionInBit() const
{
   int res = MLX90640_GetCurResolution(MLX90640_address);
   switch(res) {
    case 0: return 16;
    case 1: return 17;
    case 2: return 18;
    case 3: return 19;
    default : return 0;
   }
}

bool MLXCamera::isInterleaved() const
{
   return MLX90640_GetCurMode(MLX90640_address) == 0;
}

bool MLXCamera::isChessMode() const
{
   return MLX90640_GetCurMode(MLX90640_address) == 1;
}

void MLXCamera::setFixedTemperatureRange()
{
  fixedTemperatureRange = true;
  minTemp = 20.0;
  maxTemp = 45.0;
}

void MLXCamera::setDynamicTemperatureRange()
{
  fixedTemperatureRange = false;
}

void MLXCamera::readImage()
{
#ifndef DEBUG_INTERPOLATION
  readPixels();
  setTempScale();
#endif
}

void MLXCamera::readPixels()
{
  const float emissivity = 0.95;

  for (byte x = 0 ; x < 2 ; x++) //Read both subpages
  {
    uint16_t mlx90640Frame[834];
    int status = MLX90640_GetFrameData(MLX90640_address, mlx90640Frame);
    if (status < 0)
    {
      if (status == -8)
      {
        // Could not aquire frame data in certain time, I2C frequency may be too low
        Serial.println("GetFrame Error: could not aquire frame data in time");
      }
      else
      {
        Serial.printf("GetFrame Error: %d\n", status);
      }
    }

    const long start = millis(); 
    
    const float Ta = MLX90640_GetTa(mlx90640Frame, &mlx90640);    
    const float tr = Ta - TA_SHIFT; //Reflected temperature based on the sensor ambient temperature  
    
    MLX90640_CalculateTo(mlx90640Frame, &mlx90640, emissivity, tr, rawPixels);

    Serial.printf(" Calculate: %d\n", millis() - start);
  }
}

uint16_t MLXCamera::getFalseColor(float value) const
{
    // Heatmap code borrowed from: http://www.andrewnoske.com/wiki/Code_-_heatmaps_and_color_gradients
    static float color[][3] = { {0,0,0}, {0,0,255}, {0,255,0}, {255,255,0}, {255,0,0}, {255,0,255} };
//    static const float color[][3] = { {0,0,20}, {0,0,100}, {80,0,160}, {220,40,180}, {255,200,20}, {255,235,20}, {255,255,255} };

    static const int NUM_COLORS = sizeof(color) / sizeof(color[0]);
    value = (value - minTemp) / (maxTemp-minTemp);
    
    if(value <= 0.f)
    {
      return tft.color565(color[0][0], color[0][1], color[0][2]);
    }
      
    if(value >= 1.f)
    {
      return tft.color565(color[NUM_COLORS-1][0], color[NUM_COLORS-1][1], color[NUM_COLORS-1][2]);
    }

    value *= NUM_COLORS-1;
    const int idx1 = floor(value);
    const int idx2 = idx1+1;
    const float fractBetween = value - float(idx1);
  
    const byte ir = ((color[idx2][0] - color[idx1][0]) * fractBetween) + color[idx1][0];
    const byte ig = ((color[idx2][1] - color[idx1][1]) * fractBetween) + color[idx1][1];
    const byte ib = ((color[idx2][2] - color[idx1][2]) * fractBetween) + color[idx1][2];

    return tft.color565(ir, ig, ib);
}

uint16_t MLXCamera::getColor(float val) const
{
  /*
    pass in value and figure out R G B
    several published ways to do this I basically graphed R G B and developed simple linear equations
    again a 5-6-5 color display will not need accurate temp to R G B color calculation
    equations based on
    http://web-tech.ga-usa.com/2012/05/creating-a-custom-hot-to-cold-temperature-color-gradient-for-use-with-rrdtool/index.html
    
  */

  byte red   = constrain(255.0 / (c - b) * val - ((b * 255.0) / (c - b)), 0, 255);
  byte green = 0;
  byte blue  = 0;

  if ((val > minTemp) & (val < a)) {
    green = constrain(255.0 / (a - minTemp) * val - (255.0 * minTemp) / (a - minTemp), 0, 255);
  }
  else if ((val >= a) & (val <= c)) {
    green = 255;
  }
  else if (val > c) {
    green = constrain(255.0 / (c - d) * val - (d * 255.0) / (c - d), 0, 255);
  }
  else if ((val > d) | (val < a)) {
    green = 0;
  }

  if (val <= b) {
    blue = constrain(255.0 / (a - b) * val - (255.0 * b) / (a - b), 0, 255);
  }
  else if ((val > b) & (val <= d)) {
    blue = 0;
  }
  else if (val > d) {
    blue = constrain(240.0 / (maxTemp - d) * val - (d * 240.0) / (maxTemp - d), 0, 240);
  }

  // use the displays color mapping function to get 5-6-5 color palet (R=5 bits, G=6 bits, B-5 bits)
  return tft.color565(red, green, blue);
}

void MLXCamera::setTempScale()
{
  if (fixedTemperatureRange)
    return;

  minTemp = 255.f;
  maxTemp = 0.f;

  for (int i = 0; i < 768; i++) {
    minTemp = min(minTemp, filteredPixels[i]);
    maxTemp = max(maxTemp, filteredPixels[i]);
  }

  setAbcd();
}

// Function to get the cutoff points in the temp vs RGB graph.
void MLXCamera::setAbcd()
{
  a = minTemp + (maxTemp - minTemp) * 0.2121;
  b = minTemp + (maxTemp - minTemp) * 0.3182;
  c = minTemp + (maxTemp - minTemp) * 0.4242;
  d = minTemp + (maxTemp - minTemp) * 0.8182;
}

void MLXCamera::drawImage(const float *pixelData, int width, int height, int scale) const
{
  for (int y=0; y<height; y++) {
    for (int x=0; x<width; x++) {
      tft.fillRect(tft.cursor_x + x*scale, tft.cursor_y + 10 + y*scale, scale, scale, getFalseColor(pixelData[(width-1-x) + (y*width)]));
    }
  }
}

// exponential filtering https://en.wikipedia.org/wiki/Exponential_smoothing
void MLXCamera::denoiseRawPixels(const float smoothingFactor) const
{
  const long start = millis();

  for (int i = 0; i < 768; i++)
      filteredPixels[i] = rawPixels[i] * smoothingFactor + filteredPixels[i] * (1.f - smoothingFactor);

  Serial.printf("Denoising: %d ", millis() - start);
}

void MLXCamera::drawImage(int scale, InterpolationType interpolationType) const
{
  denoiseRawPixels(0.4f);
  
  if (interpolationType == InterpolationType::eNone) {
    drawImage(filteredPixels, 32, 24, scale);
  }
  else {
    const int upscaleFactor = 3;
    const int newWidth  = (32 - 1) * upscaleFactor + 1;
    const int newHeight = (24 - 1) * upscaleFactor + 1;
    static float upscaled[newWidth * newHeight];

    if (interpolationType == InterpolationType::eLinear)
      interpolate_image_bilinear(filteredPixels, 24, 32, upscaled, newHeight, newWidth, upscaleFactor);
    else
      interpolate_image_bicubic(filteredPixels, 24, 32, upscaled, newHeight, newWidth, upscaleFactor);

    drawImage(upscaled, newWidth, newHeight, scale);
  }
}

void MLXCamera::drawLegendGraph() const
{
  const int legendSize = 15;    
  const float inc = (maxTemp - minTemp) / (tft.height() - 24 - 20);
  int j = 0;
  for (float ii = maxTemp; ii >= minTemp; ii -= inc)
    tft.drawFastHLine(tft.width() - legendSize - 6, tft.cursor_y + 34 + j++, legendSize, getFalseColor(ii));
}
 
void MLXCamera::drawLegendText() const
{
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(tft.width() - 25, tft.height() - 10);
  tft.print(String(minTemp).substring(0, 4));
  tft.setCursor(tft.width() - 25, 20);
  tft.print(String(maxTemp).substring(0, 4));
}

// Draw a circle + measured value.
void MLXCamera::drawCenterMeasurement() const
{
  return;
  // Mark center measurement
  tft.drawCircle(120, 8+84, 3, TFT_WHITE);

  // Measure and print center temperature
  const float centerTemp = (pixels[383 - 16] + pixels[383 - 15] + pixels[384 + 15] + pixels[384 + 16]) / 4;
  tft.setCursor(86, 214);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextSize(2);
  tft.print(String(centerTemp).substring(0, 4) + " °C");
}
