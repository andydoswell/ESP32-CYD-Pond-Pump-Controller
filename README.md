# ESP32-CYD-Pond-Pump-Controller
A dawn/dusk pond pump controller for the cheap yellow display (CYD), compiled in Arduino IDE 2.3.6
  Features:
   - NTP time (UTC, with UK DST adjustment)
   - Sun arc & sun display elevation (day)
   - Moon & stars display (night)
   - Local temperature via WeeWX
   - Frost warning banner (<3 °C disables pumps)
   - Pump modes: ON / OFF / AUTO / AUTO+1h
   - Pump mode toggle via touch (raw X)
   - Pump mode saved/restored in NVS
   - Display: 320x240, landscape mode
   - Relays: GPIO22 (Pump1), GPIO27 (Pump2)
   - Buzzer: GPIO26
   - Backlight: GPIO21
   - 
   - This is configured to get the temperature from a locall WeeWX webpage at weezx.local/weewx
   - You'll probably need to change that!

   - Included is the User_Setup.h file needed to configure the TFT_eSPI library for use with the CYD
