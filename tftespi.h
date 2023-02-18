#include "esphome.h"
#include <TFT_eSPI.h>

class MyTFTeSPI : public Component {
    public:
        TFT_eSPI tft = TFT_eSPI();

        int cx,cy;

        bool needsRefresh = 1;

        void setRefresh() {
            needsRefresh = 1;
        }

        void setup() override {
            tft.init();
            tft.setRotation(3);

            cx = tft.width()/2;
            cy = tft.height()/2;

            tft.fillScreen(TFT_BLACK);
            tft.fillSmoothCircle(cx, cy, cy - 20, 0x000A, 0);
            tft.fillSmoothCircle(cx, cy, cy - 40, 0xF00A, 0x000A);
            //ESP_LOGD("custom", "initialized tft eSPI <<<<<<<<<<<<<<<<<<<");
        }

        void tempArc( int setpoint, int current ) {
            //ESP_LOGD("custom", "tempArc start set:%i now:%i <<<<<<<<<<<<<<<<<<<", setpoint, current);
            if (200 > current && current > 0) {
                //ESP_LOGD("custom", "tempArc validated <<<<<<<<<<<<<<<<<<<");
                #define arcwidth 20

                // draw setpoint arc
                int r = cy-10;
                tft.drawArc(      cx, cy, r+1, r-arcwidth-1,   40+(setpoint*5), 40, TFT_BLACK, 0, false);
                tft.drawSmoothArc(cx, cy, r,   r-arcwidth, 40, 40+(setpoint*5), TFT_BLUE, 0, false);

                // draw current temperature arc
                r = cy-30;
                tft.drawArc(      cx, cy, r+1, r-arcwidth-1, 40+(current*5), 40, TFT_BLACK, 0, false);
                tft.drawSmoothArc(cx, cy, r+1, r-arcwidth-1, 40, 40+(current*5), TFT_SKYBLUE, 0, false);
            }
        }
        void loop() override {
            static int t=millis();
            static float temperature = id(ds18b20).state;
            if (id(ds18b20).state != temperature) {
                temperature = id(ds18b20).state;
                setRefresh();
                tempArc(30, temperature);
            }
            if (millis() > t+60000 || needsRefresh) {
                needsRefresh = 0;
                t=millis();
                temperature = id(ds18b20).state;

                tft.setTextFont(7);

                if (id(btn_center).state) {
                    tft.setTextColor(TFT_YELLOW, 0xF00A);
                } else {
                    tft.setTextColor(TFT_WHITE, 0xF00A);
                }

                if (temperature>-20 and temperature<80) {
                    tft.setCursor(160-32,100);
                    float f = (float)(32.0+(temperature*1.8));
                    tft.print( (int)f );
                    //ESP_LOGD("custom", "tft espi loop");
                }

            }
        }
};
