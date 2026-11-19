#include "config.h"

// DA CAMBIARE IN BASE ALLA TUA POSIZIONE !!!
int getWindDirectionDegrees(int adc) {
  if (adc < 20 || adc > 820) return -1;
  if (adc < 155) return 0;
  if (adc < 245) return 45;
  if (adc < 365) return 90;
  if (adc < 475) return 315;
  if (adc < 585) return 135;
  if (adc < 648) return 270;
  if (adc < 698) return 225;
  return 180;
}
