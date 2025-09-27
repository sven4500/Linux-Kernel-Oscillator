make clean
make kbuild
#sudo fuser -v /dev/snd/*
#systemctl --user stop pipewire pipewire-pulse
#systemctl --user stop pulseaudio
/usr/sbin/rmmod -f ex_oscillator
/usr/sbin/insmod ./build/ex_oscillator.ko
