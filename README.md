# Bible_clock

This is a project I have been working on and off on for awhile.

The goal is to make something similar to this: https://www.thebibleclock.com/

But instead of charging $200, that is what I believe the price is as of writing this, I wanted to make one much cheaper.

![image](https://github.com/user-attachments/assets/e1512bc1-f9e2-4828-8041-c070e58c6b9f)

^ pricing is revealed if you give them your email.

I also wanted to try to make mine "available" before theirs, not in the case of I am actively selling them but meaning the guide is available.

You should be able to use the code in the boaz tf test folder to get the bible verses showing up on your display. You will need to add your own wifi and password to get it to work on your network.

The python program is designed to help you select verses I have also included the .csv file I used for mine.

You can use the website to generate the .bin files which you then drop onto the micro sd card, 32gb reccomended.



https://www.elecrow.com/wiki/CrowPanel_ESP32_E-Paper_4.2-inch_Arduino_Tutorial.html#upload-the-code
^ This is the settings in the Arduino IDE

"ESP32S3 Dev Module", and the "Partition Scheme" select "Huge APP (3MB No OTA/1MB SPIFFS)", "PSRAM" select "OPI PSRAM".
