# Vetted first script. Transport enforces HTTPS host/path/size/deadline in C.
import json
import dunlin

def quote(value):
    return ''.join('%{:02X}'.format(b) for b in value.encode('utf-8'))

def weather(city):
    geo = json.loads(dunlin.http_get('https://geocoding-api.open-meteo.com/v1/search?name=' + quote(city) + '&count=1&language=zh&format=json'))
    places = geo.get('results', [])
    if not places:
        raise ValueError('city not found; specify a city name')
    place = places[0]
    latitude = place['latitude']
    longitude = place['longitude']
    url = 'https://api.open-meteo.com/v1/forecast?latitude=' + str(latitude) + '&longitude=' + str(longitude) + '&current=temperature_2m,relative_humidity_2m,apparent_temperature,precipitation,weather_code,wind_speed_10m&timezone=auto&forecast_days=1'
    data = json.loads(dunlin.http_get(url))
    current = data['current']
    if 'time' not in current or 'temperature_2m' not in current:
        raise ValueError('weather response lacks current time or temperature')
    return json.dumps({'source':'Open-Meteo','runtime':'MicroPython','city':place['name'],'country':place.get('country',''),'latitude':latitude,'longitude':longitude,'timezone':data.get('timezone',''),'current':current,'units':data['current_units'],'note':'Weather model estimate, not an onboard sensor measurement'})
