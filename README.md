# InfinitESP
ESP32 implementation of Carrier thermostat RS485 "ABCD bus"

Placeholder for an esp32 firmware implementation of Carrier's serial RS485 protocol to automate Infinity Touch thermostats.

Prototype is being built on the esp32-s3-box-lite
[esp32-box-lite](https://amzn.to/3PqKc7E) 

<a href="https://amzn.to/3PqKc7E">
  <img src="https://user-images.githubusercontent.com/177510/207661664-22b4837d-06b4-430d-8025-3dc1fda6cab4.png">
  <img src="https://user-images.githubusercontent.com/177510/207665453-a84ce541-10b1-4319-99f0-f199573f7f49.png">
</a>



### Goals are (roughly in priority order):

1. Parse incoming RS485 state
2. Control state (current setpoints & schedule) via serial writes, similar to what the [Infinitive](https://github.com/acd/infinitive) project does for legacy stats
3. Implement a Home Assistant Climate device via [ESPHome](https://esphome.io)
4. Provide basic LCD thermostat interface
    * potentially a spin-off Home Assistant physical thermostat interface
6. Autoconfigure wifi since the thermostat freely gives out wifi credentials 🤯
7. Provide web interface
8. Play nicely with [Infinitude](https://github.com/lvgl/lvgl)
   * Act as serial/tcp bridge
   * Serve as NAT gateway
