# InfinitESP
ESP32 implementation of Carrier thermostat RS485 "ABCD bus"

Placeholder for an [esp32-box-lite](https://amzn.to/3PqKc7E) firmware implementation of Carrier's serial RS485 protocol to automate Infinity Touch thermostats.

<a href="https://amzn.to/3PqKc7E">
  <img src="https://user-images.githubusercontent.com/177510/207661664-22b4837d-06b4-430d-8025-3dc1fda6cab4.png">
  <img src="https://user-images.githubusercontent.com/177510/207665453-a84ce541-10b1-4319-99f0-f199573f7f49.png">
</a>



### Goals are (roughly in priority order):

1. Parse incoming RS485 state
2. Control state (current setpoints & schedule) via serial writes, similar to what the [Infinitive](https://github.com/acd/infinitive) project does for legacy stats
3. Provide basic LCD thermostat interface
probably based on the [LVGL](https://github.com/lvgl/lvgl) thermostat example

![image](https://user-images.githubusercontent.com/177510/207666737-81bc9be4-353f-4c83-9145-7f2431edc431.png)

4. Implement a Home Assistant standard Climate device.
   * Either as an MQTT climate device 
   * or a custom climate device. 
   * Ideally as an [ESPHome](https://esphome.io) integration
6. OpenSCAD / STL file to contain necessary RS485 module.
7. Autoconfigure wifi since the thermostat freely gives out wifi credentials 🤯
8. Provide web interface
9. Play nicely with [Infinitude](https://github.com/lvgl/lvgl)
   * Act as serial/tcp bridge
   * Serve as NAT gateway
1. ESP Rainmaker app/cloud interface
