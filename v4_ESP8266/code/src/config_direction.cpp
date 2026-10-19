#include "config.h"

// DA CAMBIARE IN BASE ALLA TUA POSIZIONE !!!
int getWindDirectionDegrees(int adc) {
  if (adc >= 40 && adc <= 150) return 0;
  if (adc >= 160 && adc <= 240) return 45;
  if (adc >= 250 && adc <= 350) return 90;
  if (adc >= 380 && adc <= 450) return 315;
  if (adc >= 500 && adc <= 580) return 135;
  if (adc >= 590 && adc <= 645) return 270;
  if (adc >= 650 && adc <= 695) return 225;
  if (adc >= 700 && adc <= 750) return 180;
  return -1;
}