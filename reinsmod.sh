make clean
make kbuild
#sudo fuser -v /dev/snd/*
#systemctl --user stop pipewire pipewire-pulse
#systemctl --user stop pulseaudio
# NOTE: -f принудительная выгрузка, так делать не надо! Если выгрузить драйвер
# уже открытый в пользовательском пространстве, будет плохо!
# TODO: иногда получаю ошибку rmmod: ERROR: Module ex_oscillator is in use?
/usr/sbin/rmmod -f ex_oscillator
/usr/sbin/insmod ./build/ex_oscillator.ko
