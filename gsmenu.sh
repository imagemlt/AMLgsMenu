#!/bin/bash

if [ -f /storage/digitalfpv/wfb.conf ]; then
	. /storage/digitalfpv/wfb.conf
else
	. /flash/wfb.conf
fi

if [ -f /storage/digitalfpv/AMLgsMenu.new ]; then
	mv /storage/digitalfpv/AMLgsMenu.new /storage/digitalfpv/AMLgsMenu
fi

killall splash-image

alias kodi-start='systemctl start kodi2'


echo ${ground_res} >/sys/class/display/mode
/storage/digitalfpv/AMLgsMenu -t /storage/digitalfpv/fonts/ZC0005-Regular-2.ttf -T /storage/digitalfpv/fonts/LXGWWenKaiMono-Regular.ttf 

