#include <Arduino.h>
#include "config.h"

#if defined(RUN_TEST_LEAF)
  #include "test_leaf.h"
#elif defined(RUN_TEST_SOIL)
  #include "test_soil.h"
#elif defined(RUN_TEST_CC1101)
  #include "test_cc1101.h"
#elif defined(RUN_MAIN)
  #include "main_app.h"
#endif

void setup() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  delay(100);

  #if defined(RUN_TEST_LEAF)
    setup_test_leaf();
  #elif defined(RUN_TEST_SOIL)
    setup_test_soil();
  #elif defined(RUN_TEST_CC1101)
    setup_test_cc1101();
  #elif defined(RUN_MAIN)
    setup_main();
  #endif
}

void loop() {
  #if defined(RUN_TEST_LEAF)
    loop_test_leaf();
  #elif defined(RUN_TEST_SOIL)
    loop_test_soil();
  #elif defined(RUN_TEST_CC1101)
    loop_test_cc1101();
  #elif defined(RUN_MAIN)
    loop_main();
  #endif
}
