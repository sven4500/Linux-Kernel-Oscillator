make clean
make kbuild
#sudo fuser -v /dev/snd/*
#systemctl --user stop pipewire pipewire-pulse
#systemctl --user stop pulseaudio
# NOTE: -f forces unload, this should not be done! If a driver already opened
# from user space is unloaded, bad things will happen!
# TODO: sometimes getting error rmmod: ERROR: Module ex_oscillator is in use?
/usr/sbin/rmmod -f ex_oscillator
/usr/sbin/insmod ./build/ex_oscillator.ko
