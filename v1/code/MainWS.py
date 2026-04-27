#!/usr/bin/python3

import time, datetime, math, statistics
from gpiozero import Button, MCP3008
import BME680, W_data

#SERVER
THINGSBOARD_HOST = 'YOUR_IP'
TOKEN = 'YOUR_THINGSBOARD_TOKEN1'
TOKEN_BME680 = 'YOUR_THINGSBOARD_TOKEN2'
THINGSBOARD_MQTT_PORT = 1883
KEEP_ALIVE = 60

#UPDATE
interval = 580
wind_interval = 5
invioDati = False
startTime = time.time()

#SPEED_DATA
speed_sensor = Button(5)
speed_count = 0
radius_cm = 9.0
CM_IN_A_KM = 100000.0
SECS_IN_AN_HOUR = 3600
ADJUSTMENT = 1.18
store_speeds = []

#RAIN_DATA
rain_sensor = Button(6)
rain_count = 0
BUCKET_SIZE = 0.2794

#DIRECTION_DATA
adc = MCP3008(channel=0)
direction_count = 0
store_direction = []
store_direction_gust = []

#CHANGE WITH YOUR VALUES!!!
volts={0.4:315.0,
       1.4:337.5,
       1.2:0.0,
       2.8:22.5,
       2.7:45.0,
       2.9:67.5,
       2.2:90.0,
       2.5:112.5,
       1.8:135.0,
       2.0:157.5,
       0.7:180.0,
       0.8:202.5,
       0.1:225.0,
       0.3:247.5,
       0.2:270.0,
       0.6:292.5}

#SPEED_METHODS
def spin():
  global speed_count
  speed_count = speed_count + 1

def calculate_speed(time_sec):
  global speed_count
  global gust
  circumference_cm = (2 * math.pi) * radius_cm
  rotations = speed_count / 2.0
  dist_km = (circumference_cm * rotations) / CM_IN_A_KM
  km_per_sec = dist_km / time_sec
  km_per_hour = km_per_sec * SECS_IN_AN_HOUR
  return km_per_hour * ADJUSTMENT

#DIRECTION_METHODS
def get_direction_average(angles):
  sin_sum = 0.0
  cos_sum = 0.0

  for angle in angles:
      r = math.radians(angle)
      sin_sum += math.sin(r)
      cos_sum += math.cos(r)

  flen = float(len(angles))
  if flen == 0:
    print("flen = 0")
  else:
    s = sin_sum / flen
    c = cos_sum / flen
    arc = math.degrees(math.atan(s / c))
    average = 0.0

    if s > 0 and c > 0:
        average = arc
    elif c < 0:
        average = arc + 180
    elif s < 0 and c > 0:
        average = arc + 360

    return 0.0 if average == 360 else average

#RAIN_METHODS
def bucket_tipped():
  global rain_count
  rain_count = rain_count + 1

#RESETS
def reset_spin():
  global speed_count
  speed_count = 0

def reset_rain():
  global rain_count
  rain_count = 0


while True:
	if(time.time() - startTime < interval and invioDati == False):
		invioDati = True
		startTime2 = time.time()
		lastSendAt = time.time() + 1

	if(invioDati):
		if(time.time() - startTime2 < interval):
			if (time.time() - lastSendAt > wind_interval):
				speed_sensor.when_pressed = spin
				final_speed = calculate_speed(interval)
				store_speeds.append(final_speed)

				gust = max(store_speeds)
				wind_speed = statistics.mean(store_speeds)
				direction = round(adc.value*3.3,1)

				if not direction in volts:
					print("Nessun valore")
				else:
					store_direction.append(volts[direction])

				rain_sensor.when_pressed = bucket_tipped

				if gust == max(store_speeds):
					store_direction_gust.append(direction)

				direction_gust = store_direction_gust[-1]

				lastSendAt = time.time()
		else:
			#TIME
			now = datetime.datetime.now()

			year = '{:02d}'.format(now.year)
			month = '{:02d}'.format(now.month)
			day = '{:02d}'.format(now.day)
			hour = '{:02d}'.format(now.hour)
			minute = '{:02d}'.format(now.minute)
			second = '{:02d}'.format(now.second)

			#SENSORI
			bme680 = BME680.getDataBME680()

			speedWS = {'Spin' : speed_count, 'Speed': wind_speed, 'Gust': gust}
			directionWS = {'Direction' : get_direction_average(store_direction), 'DirectionGust': direction_gust}
			rainWS = {'RainCount': rain_count, 'Rain': (rain_count * BUCKET_SIZE)}
			tempo = {'Time' : '{}/{}/{} {}:{}:{}'.format(day, month, year, hour, minute, second)}

			#SEND_DATA
			if speedWS !=0:
				W_data.main(speedWS, TOKEN, THINGSBOARD_HOST, THINGSBOARD_MQTT_PORT, KEEP_ALIVE)
			if directionWS !=0:
				W_data.main(directionWS, TOKEN, THINGSBOARD_HOST, THINGSBOARD_MQTT_PORT, KEEP_ALIVE)
			if rainWS !=0:
				W_data.main(rainWS, TOKEN, THINGSBOARD_HOST, THINGSBOARD_MQTT_PORT, KEEP_ALIVE)
			if bme680 !=0:
				W_data.main(bme680, TOKEN_BME680, THINGSBOARD_HOST, THINGSBOARD_MQTT_PORT, KEEP_ALIVE)
			if tempo !=0:
				W_data.main(tempo, TOKEN, THINGSBOARD_HOST, THINGSBOARD_MQTT_PORT, KEEP_ALIVE)

        	#RESET
			reset_spin()
			store_speeds.clear()
			reset_rain()
			store_direction.clear()
			store_direction_gust.clear()
			startTime = time.time()
			invioDati = False
