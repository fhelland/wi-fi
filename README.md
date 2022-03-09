# wi-fi

This is code for esp32 wi-fi module.

It contains a TCP-server to uart bridge, wi-fi manager and http server. The http server has a web-interface for downloading files from a sd-card, uploading firmware over OTA (over-the-air) and connecting to wi-fi networks.

The IP-adress of the device is 10.10.0.1 by default. This can be changed in menuconfig (to ex 192.168.4.1).

Enter the ip adress in a browser to get to the user interface. 

mDNS is enabled to be able to reach the device by hostname instead of entering ip-adress. This is useful if the device is connected to another network and IP-adress is unknown. Enter hostname.local to get to the device. Default hostname is esp32. This can be changed in wifi_manager.h. The wi-fi module will try and get node description from uart during boot. This will be used to create a hostname and wi-fi ssid. The name is visilbe on the wi-fi page of the user interface.
