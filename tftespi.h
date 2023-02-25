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
                float scale = 280.0/100.0;
                tft.drawArc(      cx, cy, r+1, r-arcwidth-1,   40+(setpoint*scale), 40, TFT_BLACK, 0, false);
                tft.drawSmoothArc(cx, cy, r,   r-arcwidth, 40, 40+(setpoint*scale), TFT_BLUE, 0, false);

                // draw current temperature arc
                r = cy-30;
                tft.drawArc(      cx, cy, r+1, r-arcwidth-1, 40+(current*scale), 40, TFT_BLACK, 0, false);
                tft.drawSmoothArc(cx, cy, r+1, r-arcwidth-1, 40, 40+(current*scale), TFT_SKYBLUE, 0, false);
            }
        }
        void loop() override {
            static int t=millis();
            static float temperature = id(climate_clone_current_temperature).state;
            static float setpoint = id(climate_clone_setpoint).state;
            if (id(climate_clone_current_temperature).state != temperature) {
                temperature = id(climate_clone_current_temperature).state;
                setpoint = id(climate_clone_setpoint).state;
                setRefresh();
                tempArc(setpoint, temperature);
            }
            if (millis() > t+60000 || needsRefresh) {
                needsRefresh = 0;
                t=millis();

                tft.setTextFont(7);

                if (id(btn_center).state) {
                    tft.setTextColor(TFT_YELLOW, 0xF00A);
                } else {
                    tft.setTextColor(TFT_WHITE, 0xF00A);
                }

                if (temperature>-20 and temperature<80) {
                    tft.setCursor(160-32,100);
                    //float f = (float)(32.0+(temperature*1.8));
                    tft.print( (int)temperature );
                    tft.setTextFont(4);
                    tft.setCursor(160-15,150);
                    tft.println( (int)setpoint );
                    //ESP_LOGD("custom", "tft espi loop");
                }

            }
        }
};
