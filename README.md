# InfinitESP
ESP32 implementation of Carrier thermostat RS485 "ABCD bus"

Placeholder for an [esp32-box-lite](https://amzn.to/3PqKc7E) firmware implementation of Carrier's serial RS485 protocol.

<a href="https://amzn.to/3PqKc7E"><img src="https://user-images.githubusercontent.com/177510/207661664-22b4837d-06b4-430d-8025-3dc1fda6cab4.png"></a>

Goals are:

1. autoconfigure wifi since the thermostat freely gives out wifi credentials 🤯
2. control current temperature and schedule via serial writes, similar to what the Infinitive project does for legacy stats
3. Provide basic LCD thermostat interface
4. Implement a standard Climate device which can be used by Home Assistant. Either as an MQTT climate device or a custom climate device.
5. OpenSCAD / STL file to contain necessary RS485 module.
