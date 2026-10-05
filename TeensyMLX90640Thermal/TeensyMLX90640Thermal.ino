#include <Wire.h>
#include <Adafruit_MLX90640.h>

Adafruit_MLX90640 mlx;
float mlx90640Frame[768];

void setup() {
  Serial.begin(460800);
  delay(2000); // Allow native USB to initialise

  Wire.begin();
  Wire.setClock(100000); // Keep at stable 100 kHz

  if (!mlx.begin()) {
    while (1) { delay(1000); }
  }

  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX90640_4_HZ); 
}

void loop() {
  int status = mlx.getFrame(mlx90640Frame);
  if (status < 0) {
    return; // Skip bad frame reads
  }

  // Stream full frame to Python GUI
  Serial.print(F("FRAME:"));
  for (int i = 0; i < 768; i++) {
    Serial.print(mlx90640Frame[i], 2);
    if (i < 767) {
      Serial.print(',');
    }
  }
  Serial.println();
}