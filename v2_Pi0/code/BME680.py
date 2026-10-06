import time, board
import adafruit_bme680

i2c = board.I2C()
bme680 = adafruit_bme680.Adafruit_BME680_I2C(i2c)

def getDataBME680():

  sensor_dataBME680 = {'Temperature': bme680.temperature,
                       'Gas': bme680.gas,
                       'Humidity': bme680.relative_humidity,
                       'Pressure': bme680.pressure,
                       'Altitude': bme680.altitude
                      }

  return sensor_dataBME680
