#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>        // struct cdev, ...
#include <linux/fixp-arith.h>  // __fixp_sin32, ...
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>   // s16, u64, size_t, ...
#include <sound/asound.h>  // snd_pcm_uframes_t, ...
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>  // SNDRV_PCM_TRIGGER_START, SNDRV_PCM_TRIGGER_STOP, ...
#include <sound/pcm_params.h>

// NOTE:
// https://www.kernel.org/doc/html/v4.15/sound/kernel-api/alsa-driver-api.html
// NOTE:
// https://github.com/torvalds/linux/blob/master/include/linux/fixp-arith.h
// NOTE:
// https://github.com/eclipse-cdt/cdt/tree/main/FAQ#whats-the-best-way-to-set-up-the-cdt-to-navigate-linux-kernel-source
// NOTE:
// https://embetronicx.com/tutorials/linux/device-drivers/ioctl-tutorial-in-linux/

// NOTE: alsaloop -C hw:1,0 -P hw:0,0 -c 2 -f S16_LE -r 48000
// NOTE: speaker-test -D hw:1,0 -c 1 -t sine -r 48000 -f 400 | aplay -D hw:0,0
// -r 48000 -f S16_LE -c 2 -B 100000 -v

#define DEVICE_NAME "ksound_device"
#define CLASS_NAME "ksound_class"
#define DRIVER_NAME "ksound"
#define CARD_NAME "KernelSoundCard"

#define MYDEVMAGIC 's'
#define CMDADDWAVE _IOW(MYDEVMAGIC, 0, u32)
#define CMDREMOVEWAVE _IOW(MYDEVMAGIC, 1, u32)

// NOTE: амплитуда 7 бит (128 знач., валидные 0..100), фаза 9 бит (512 знач.,
// валидные 0..360), частота 16 бит (64к знач., валидные 0..48000)
#define MAKEWAVE(amp, phase, freq) \
    (((amp)&0x7f) | (((phase)&0x1ff) << 7) | (((freq)&0xffff) << 16))

#define GETWAVEAMP(w) ((w)&0x7f)
#define GETWAVEPHASE(w) (((w) >> 7) & 0x1ff)
#define GETWAVEFREQ(w) (((w) >> 16) & 0xffff)

#define SETWAVEAMP(wave, amp) (((wave)&0xFFFFFF80) | ((amp)&0x7f))
#define SETWAVEPHASE(wave, phase) (((wave)&0xFFFF007F) | (((phase)&0x1ff) << 7))
#define SETWAVEFREQ(wave, freq) (((wave)&0x0000FFFF) | (((freq)&0xffff) << 16))

/*
 * Описание виртуальной карты. К типам принадлежащим этому модулу добавляю
 * префикс ksound_.
 */
struct ksound_card {
    struct snd_card *card;
    struct hrtimer timer;
    struct snd_pcm_substream *substream;
    atomic_t running;
    snd_pcm_uframes_t hw_ptr;  // указатель проигрываемое место в бфере
};

static DEFINE_MUTEX(mutex);

/*
 * Описывает PCM поток
 */
static struct snd_pcm_hardware snd_ksound_capture_hw = {
    .info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
             SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID),
    .formats = SNDRV_PCM_FMTBIT_S16_LE,
    .rates = SNDRV_PCM_RATE_48000,
    .rate_min = 48000,  // минимальная частота дискретизации (runtime->rate)
    .rate_max = 48000,  // максимальная частота дискретизации (runtime->rate)
    .channels_min = 2,  // минимальное и максимальное количество каналов
    .channels_max = 2,  // на моей системе вывод динамиков только в 2 канала
    .buffer_bytes_max = 128 * 1024,  // BUFFER_SIZE,
    .period_bytes_min = 256,
    .period_bytes_max = 16 * 1024,  // BUFFER_SIZE,
    .periods_min = 4,
    .periods_max = 1024,
};

// пока кажется что с int работать проще, может понадобиться позже?
/*struct ksound_wave
 * {
 *    int frequency;  // частота в Гц
 *    int phase;      // текущая фаза
 *    int amplitude;  // амплитуда - возможно потом?
 * };*/

// NOTE: static u32 sound_waves[] = { MAKEWAVE(100, 0, 480) };
static u32 *sound_waves = NULL;
static int wave_count = 0;

/*
 * Генерирует один пилообразный сигнал.
 */
static void make_saw_wave(s16 *samples, size_t count, int rate, u32 wave) {
    size_t i;
    // TODO: int const amp = GETWAVEAMP(wave);
    int phase = GETWAVEPHASE(wave);
    int const freq = GETWAVEFREQ(wave);

    for (i = 0; i < count; i++) {
        // NOTE: 65536, 32768, 16384, 8192, 4096
        s16 const sample = (s16)(((phase * 8192) / rate) - 4096);

        phase += freq;
        if (phase >= rate) phase -= rate;

        // NOTE: записать дискрету L+R, не работает с другим количество каналов
        samples[i * 2 + 0] = sample;
        samples[i * 2 + 1] = sample;
    }
}

/*
 * Генерирует один гармонический сигнал.
 */
static void make_sine_wave(s16 *samples, size_t count, int rate, u32 wave) {
    int i;
    // TODO: int const amp = GETWAVEAMP(wave);
    int phase = GETWAVEPHASE(wave);
    int const freq = GETWAVEFREQ(wave);
    int const step = (360 * freq) / rate;

    for (i = 0; i < count; i++) {
        // NOTE: -0x7fffffff .. +0x7fffffff
        s32 const sample = __fixp_sin32(phase) >> 16;

        phase += step;
        if (phase >= 360) phase -= 360;

        samples[i * 2 + 0] = (s16)sample;
        samples[i * 2 + 1] = (s16)sample;
    }
}

/*
 * Генерирует несколько гармонических сигналов. Сигнал описан в структуре
 * sine_wave.
 */
static void make_sine_waves(s16 *samples, size_t sample_count, int rate,
                            u32 *waves, int wave_count) {
    int i, j;

    for (i = 0; i < sample_count; i++) {
        s32 mixed = 0;

        for (j = 0; j < wave_count; j++) {
            u32 const wave = waves[j];

            // TODO: int const amp = GETWAVEAMP(wave);
            int phase = GETWAVEPHASE(wave);
            int const freq = GETWAVEFREQ(wave);
            int const step = (360 * freq) / rate;

            s32 const sample = __fixp_sin32(phase) >> 16;

            phase += step;
            if (phase >= 360) phase -= 360;

            // NOTE: нужно сохранить новую фазу, иначе волна не развивается
            waves[j] = SETWAVEPHASE(wave, phase);

            mixed += sample;
        }

        if (wave_count > 0) mixed /= wave_count;

        samples[i * 2 + 0] = (s16)mixed;
        samples[i * 2 + 1] = (s16)mixed;
    }
}

/*
 * Обработка сэмплов буфера. runtime->rate частота дискретизации канала.
 */
static enum hrtimer_restart ksound_timer_callback(struct hrtimer *timer) {
    struct ksound_card *const card =
        container_of(timer, struct ksound_card, timer);
    struct snd_pcm_substream *const substream = card->substream;
    struct snd_pcm_runtime *const runtime = substream->runtime;

    // period - аудио фрагмент, frames - количество дискрет на фрагмент. У нас
    // 2 канала и 16 бит на канал поэтому frames_to_bytes вернёт period * 4.
    s16 *const samples = (s16 *)(runtime->dma_area + card->hw_ptr);
    size_t const period_bytes = frames_to_bytes(runtime, runtime->period_size);
    size_t const buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
    u64 period_ns;
    ktime_t const now = ktime_get();

    // NOTE: runtime->dma_bytes размер DMA области в байтах, заметил что DMA
    // область может быть чуть больше чем размер буфера
    BUG_ON(runtime->dma_bytes < buffer_bytes);
    BUG_ON(card->hw_ptr >= buffer_bytes);

    // pr_info("ksound_timer_callback hw_ptr=%lu, period=%lu, buffer=%lu,
    // dmabytes=%lu", card->hw_ptr, runtime->period_size, runtime->buffer_size,
    // runtime->dma_bytes);

    if (!atomic_read(&card->running)) return HRTIMER_NORESTART;

    // проверить что не выходим за область DMA, если выйти возможно падение
    // ядра
    mutex_lock(&mutex);

    if (buffer_bytes - card->hw_ptr >= period_bytes) {
        // TODO: после удаления последней волны её всё равно слышно. Как будто
        // в DMA буфере остаются данные. Можно ли его не перезаписывать DMA?
        make_sine_waves(samples, runtime->period_size, runtime->rate,
                        sound_waves, wave_count);
        // make_sine_wave(samples, runtime->period_size, runtime->rate,
        // MAKEWAVE(100, 0, 480));
        // make_saw_wave(samples, runtime->period_size, runtime->rate, 400,
        // 0); make_sine_wave(samples, runtime->period_size, runtime->rate,
        // &sine_waves[0]);
    }

    mutex_unlock(&mutex);

    // for (i = 0; i < runtime->period_size; i++)
    // pr_info("%d ", ptr[i]);

    // Подвинуть указатель на следующий фрагмент. Лучше переходить в начало или
    // с сохранением хвоста?
    // card->hw_ptr = (card->hw_ptr + period_bytes) % runtime->dma_bytes;
    card->hw_ptr += period_bytes;
    if (card->hw_ptr >= buffer_bytes) card->hw_ptr = 0;

    // уведомить ALSA
    snd_pcm_period_elapsed(substream);

    // Продолжительность периода в нс. Количество дискрет поделить на частоту
    // дискретизации даёт секунды, умножаем на NSEC_PER_SEC чтобы получить нс.
    period_ns = div_u64(runtime->period_size * NSEC_PER_SEC, runtime->rate);

    // Задать таймер, так тоже можно. Пока не понимаю как лучше
    // hrtimer_forward_now(timer, ns_to_ktime(period_ns))
    hrtimer_forward(timer, now, ns_to_ktime(period_ns));
    return HRTIMER_RESTART;
}

/*
 * открыть PCM поток
 */
static int snd_ksound_capture_open(struct snd_pcm_substream *substream) {
    struct ksound_card *card = substream->pcm->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;

    card->substream = substream;
    substream->private_data = card;

    // обязательно заполнить во время open, иначе ошибка открытия потока!
    runtime->hw = snd_ksound_capture_hw;

    // snd_pcm_hw_constraint_single(runtime, SNDRV_PCM_HW_PARAM_RATE,
    // SAMPLE_RATE); snd_pcm_hw_constraint_single(runtime,
    // SNDRV_PCM_HW_PARAM_CHANNELS, 2); snd_pcm_hw_constraint_single(runtime,
    // SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    // snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
    // snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
    // 64, 1*1024*1024);

    pr_info("snd_ksound_capture_open\n");
    return 0;
}

static int snd_ksound_capture_hw_params(struct snd_pcm_substream *substream,
                                        struct snd_pcm_hw_params *hw_params) {
    struct snd_pcm_runtime const *const runtime = substream->runtime;
    size_t const buffer_bytes = params_buffer_bytes(hw_params);
    size_t const alloc_bytes = ALIGN(buffer_bytes, PAGE_SIZE);

    pr_info("snd_ksound_capture_hw_params buffer_bytes=%lu, alloc_bytes=%lu\n",
            buffer_bytes, alloc_bytes);

    if (alloc_bytes <= 0) {
        pr_info("snd_ksound_capture_hw_params bad alloc_bytes=%lu\n",
                alloc_bytes);
        return EINVAL;
    }

    // NOTE: исправляется выравниванием буфера по границе страницы alloc_bytes
    // [Сб окт 11 20:22:47 2025] BUG: KASAN: vmalloc-out-of-bounds in
    // snd_pcm_hw_params+0x10ea/0x15a0 [snd_pcm] [Сб окт 11 20:22:47 2025] Write
    // of size 20480 at addr ffffc900001b7000 by task pulseaudio/1509 [Сб окт 11
    // 20:22:47 2025] CPU: 4 PID: 1509 Comm: pulseaudio Tainted: G    B OE
    // N 6.1.130 #3 [Сб окт 11 20:22:47 2025] Hardware name: innotek GmbH
    // VirtualBox/VirtualBox, BIOS VirtualBox 12/01/2006

    // NOTE: snd_pcm_lib_free_vmalloc_buffer(substream) нужно ли???

    // NOTE:
    // https://www.kernel.org/doc/html/v5.1/sound/kernel-api/writing-an-alsa-driver.html
    // return snd_pcm_lib_malloc_pages(substream, buffer_bytes);

    return snd_pcm_lib_alloc_vmalloc_buffer(substream, alloc_bytes);
}

static int snd_ksound_capture_hw_free(struct snd_pcm_substream *substream) {
    pr_info("snd_ksound_capture_hw_free\n");

    // NOTE: //
    // https://www.kernel.org/doc/html/v4.16/sound/kernel-api/writing-an-alsa-driver.html
    // return snd_pcm_lib_free_pages(substream);

    return snd_pcm_lib_free_vmalloc_buffer(substream);
}

/*
 * подготовка PCM потока
 */
static int snd_ksound_capture_prepare(struct snd_pcm_substream *substream) {
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;

    int buffer_bytes = frames_to_bytes(
        runtime, runtime->buffer_size);  // params_buffer_bytes(hw_params)

    pr_info(
        "snd_ksound_capture_prepare buffer_size=%ld, period_size=%ld, "
        "format=%d, buffer_bytes=%d\n",
        runtime->buffer_size, runtime->period_size, runtime->format,
        buffer_bytes);

    return 0;
}

/*
 * смена состояние PCM
 */
static int snd_ksound_capture_trigger(struct snd_pcm_substream *substream,
                                      int cmd) {
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;

    pr_info("snd_ksound_capture_trigger cmd=%d, dma=%p\n", cmd,
            runtime->dma_area);

    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START: {
            card->hw_ptr = 0;
            atomic_set(&card->running, 1);

            // запустить таймер
            hrtimer_init(&card->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
            card->timer.function = ksound_timer_callback;
            hrtimer_start(&card->timer, ns_to_ktime(0), HRTIMER_MODE_REL);

            return 0;
        }

        case SNDRV_PCM_TRIGGER_STOP:
            atomic_set(&card->running, 0);

            // остановить таймер
            hrtimer_cancel(&card->timer);
            return 0;

        case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
            pr_info("capture paused\n");
            return 0;

        case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
            pr_info("capture resumed\n");
            return 0;

        default:
            return EINVAL;
    }
}

/*
 * Указатель на место проигрывания в буфере. Возвращает указатель в дискретах.
 */
static snd_pcm_uframes_t snd_ksound_capture_pointer(
    struct snd_pcm_substream *substream) {
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;

    // pr_info("hw_ptr=%lu, period=%lu, bytes=%d\n",
    // card->hw_ptr, runtime->period_size, bytes_to_frames(substream->runtime,
    // card->hw_ptr));

    // похоже что ALSA подсистеме нужен указатель в дискретах, а не байтах
    return bytes_to_frames(substream->runtime, card->hw_ptr);
}

/*
 * почему этот метод магическим образом очищает поток?
 */
/*static snd_pcm_uframes_t snd_ksound_capture_pointer(struct snd_pcm_substream
 **substream)
 * {
 *	struct ksound_card *card = substream->private_data;
 *	struct snd_pcm_runtime *runtime = substream->runtime;
 *	static snd_pcm_uframes_t simulated_position = 0;
 *
 *	if (atomic_read(&card->running))
 *	{
 *		// Advance the simulated position
 *		simulated_position += runtime->period_size;
 *        //pr_info("%lu, %lu, %p", simulated_position, runtime->period_size,
 *runtime->dma_area);
 *
 *		// Wrap around at buffer size
 *		if (simulated_position >= runtime->buffer_size)
 *			simulated_position = 0;
 *
 *		// Update the card's hardware pointer
 *		card->hw_ptr = simulated_position;
 *	}
 *
 *	return card->hw_ptr;
 * }*/

/*
 * закрыть PCM поток
 */
static int snd_ksound_capture_close(struct snd_pcm_substream *substream) {
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct ksound_card *card = substream->private_data;

    card->substream = NULL;  // substream освобождается на этапе hw_free
    substream->private_data = NULL;

    pr_info("snd_ksound_capture_close\n");
    return 0;
}

/*
 * Таблица операторов PCM. Проходит цикл open, hw_params, prepare, trigger,
 * pointer?, trigger, free, close.
 */
static struct snd_pcm_ops snd_ksound_capture_ops = {
    .open = snd_ksound_capture_open,
    .close = snd_ksound_capture_close,
    .ioctl = snd_pcm_lib_ioctl,  // иначе не откроется например через alsaloop
    .hw_params = snd_ksound_capture_hw_params,
    .hw_free = snd_ksound_capture_hw_free,
    .prepare = snd_ksound_capture_prepare,
    .trigger = snd_ksound_capture_trigger,
    .pointer = snd_ksound_capture_pointer,
    //.page = snd_pcm_lib_get_vmalloc_page, // Use this for vmalloc buffers
    //.copy_user
    //.copy_kernel
};

/*
 * Реализует операцию open.
 */
static int my_open(struct inode *inode, struct file *file) {
    pr_info("unimplemented open operation\n");
    return 0;
}

/*
 * Реализует операцию ioctl.
 */
static long my_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    int const magic = _IOC_TYPE(cmd), nr = _IOC_NR(cmd);

    if (magic != MYDEVMAGIC) {
        pr_info("bad device magic %d, expected %d\n", magic, MYDEVMAGIC);
        return ENOTTY;
    }

    if (nr >= 2) {
        pr_info("no such command with index number %d\n", nr);
        return ENOTTY;
    }

    pr_info("my_ioctl cmd=0x%d, nr=%d\n", cmd, nr);

    if (cmd == CMDADDWAVE) {
        int const new_wave_count = wave_count + 1;
        int const old_wave_count = wave_count;
        u32 *const new_waves =
            kzalloc(new_wave_count * sizeof(u32), GFP_KERNEL);
        u32 *const old_waves = sound_waves;
        u32 wave;
        int amp = 0, phase = 0, freq = 0;

        if (!new_waves) {
            pr_info("my_ioctl failed to create wave buffer\n");
            return -1;
        }

        BUG_ON(old_waves == NULL && old_wave_count != 0);
        BUG_ON(new_waves == NULL);
        BUG_ON(new_wave_count < 1);

        copy_from_user(&wave, (void *)arg, sizeof(wave));

        amp = GETWAVEAMP(wave);
        phase = GETWAVEPHASE(wave);
        freq = GETWAVEFREQ(wave);

        pr_info(
            "my_ioctl add wave=0x%x, amp=%d, phase=%d, freq=%d, "
            "new_wave_count=%d, old_wave_count=%d\n",
            wave, amp, phase, freq, new_wave_count, old_wave_count);

        mutex_lock(&mutex);

        if (old_wave_count > 0)
            memcpy(new_waves, old_waves, old_wave_count * sizeof(u32));
        new_waves[new_wave_count - 1] = wave;

        sound_waves = new_waves;
        wave_count = new_wave_count;

        if (old_waves) {
            kfree(old_waves);
        }

        mutex_unlock(&mutex);
    } else if (cmd == CMDREMOVEWAVE) {
        int new_wave_count = 0;
        int const old_wave_count = wave_count;
        u32 *new_waves = NULL;
        u32 *const old_waves = sound_waves;
        u32 freq;
        int i, j;

        BUG_ON(old_waves == NULL && old_wave_count != 0);

        copy_from_user(&freq, (void *)arg, sizeof(freq));

        pr_info("my_ioctl remove freq=%d\n", freq);

        if (old_waves == NULL) {
            pr_info("my_ioctl sound waves empty\n");
            return 0;
        }

        mutex_lock(&mutex);

        // NOTE: первый проход подсчитать сколько волн исключая заданную частоту
        for (i = 0; i < old_wave_count; ++i) {
            if (GETWAVEFREQ(old_waves[i]) != freq) {
                ++new_wave_count;
            }
        }

        pr_info("new_wave_count=%d, old_wave_count=%d\n", new_wave_count,
                old_wave_count);
        BUG_ON(new_wave_count > old_wave_count);

        if (new_wave_count == 0) {
            kfree(sound_waves);

            sound_waves = NULL;
            wave_count = 0;
        } else if (new_wave_count < old_wave_count) {
            new_waves = kzalloc(new_wave_count * sizeof(u32), GFP_KERNEL);

            if (new_waves != NULL) {
                // NOTE: второй проход, выбрать только нужные волны
                for (i = 0, j = 0; i < old_wave_count; ++i) {
                    if (GETWAVEFREQ(old_waves[i]) != freq) {
                        BUG_ON(j >= new_wave_count);

                        new_waves[j] = old_waves[i];
                        ++j;
                    }
                }

                kfree(sound_waves);

                sound_waves = new_waves;
                wave_count = new_wave_count;
            }
        }

        mutex_unlock(&mutex);
    } else {
        pr_info("unknown command cmd=0x%x\n", cmd);
        BUG_ON(true);
    }

    return 0;
}

/*
 * Реализует операцию read.
 */
static ssize_t my_read(struct file *file, char __user *buf, size_t count,
                       loff_t *offset) {
    pr_info("unimplemented read operation\n");
    return 0;
}

/*
 * Реализует операцию write.
 */
static ssize_t my_write(struct file *file, char __user const *buf, size_t count,
                        loff_t *offset) {
    pr_info("unimplemented write operation\n");
    return 0;
}

static int my_release(struct inode *inode, struct file *file) {
    pr_info("unimplemented release operation\n");
    return 0;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = my_open,
    .release = my_release,
    .read = my_read,
    .write = my_write,
    .unlocked_ioctl = my_ioctl,
};

/*
 * Глобальные переменные для хранения дескрипторов устройства, карты и пр. Эти
 * значения зашиваются в поля private_data карты и pcm потока и функциям
 * следует брать эти данные оттуда поэтому эти переменные максимально внизу.
 */
static dev_t dev_num;
static struct cdev my_cdev;
static struct class *my_class;
static struct device *my_device;

static struct platform_device *pdev;
static struct snd_pcm *pcm;
static struct ksound_card *k_card;

// TODO: можно ли так инициализировать драйвер платформы?
// static struct platform_driver my_card_driver = {
//    .driver = {
//        .name = "mycard",
//    },
//    .probe  = my_card_probe,
//    .remove = my_card_remove,
//};
// module_platform_driver(my_card_driver);

/*
 * Инициализирует модуль. Создаёт новый драйвер платформы который выступает в
 * качестве родителя для ALSA карты.
 */
static int __init ksound_init(void) {
    int err;

    err = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (err < 0) {
        pr_info("failed to allocate char dev region\n");
        err = -1;
        goto __error1;
    }

    cdev_init(&my_cdev, &fops);
    my_cdev.owner = THIS_MODULE;

    err = cdev_add(&my_cdev, dev_num, 1);
    if (err < 0) {
        pr_info("failed to create characted dev\n");
        err = -1;
        goto __error2;
    }

    my_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(my_class)) {
        pr_info("failed to create class\n");
        err = -1;
        goto __error3;
    }

    // NOTE: добавляет файл /dev/ksound_device
    my_device = device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(my_device)) {
        pr_info("failed to create device\n");
        err = -1;
        goto __error4;
    }

    // NOTE: создать драйвер платформы
    pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
    if (IS_ERR(pdev)) {
        pr_info("failed to create platform device\n");
        err = -1;
        goto __error5;
    }

    // NOTE: создать виртуальную карту
    k_card = kzalloc(sizeof(*k_card), GFP_KERNEL);
    if (!k_card) {
        pr_info("failed to allocate card struct\n");
        err = ENOMEM;
        goto __error6;
    }

    // NOTE: инциализация полей структуры карты
    atomic_set(&k_card->running, 0);
    k_card->hw_ptr = 0;

    // NOTE: создать ALSA карту, в качестве родителя драйвер платформы (aplay
    // -l) для чего приватные данные (0)?
    err = snd_card_new(&pdev->dev, -1, DRIVER_NAME, THIS_MODULE, 0,
                       &k_card->card);
    if (err < 0) {
        pr_info("failed to create sound card\n");
        err = -1;
        goto __error7;
    }

    strcpy(k_card->card->driver, DRIVER_NAME);
    strcpy(k_card->card->shortname, CARD_NAME);
    sprintf(k_card->card->longname, "%s at virtual", CARD_NAME);

    // NOTE: создать pcm устройство, playback_count=0, capture_count=1
    err = snd_pcm_new(k_card->card, DRIVER_NAME, 0, 0, 1, &pcm);
    if (err < 0) {
        pr_info("failed to create pcm stream\n");
        err = -1;
        goto __error8;
    }

    strcpy(pcm->name, CARD_NAME);
    pcm->private_data = k_card;
    pcm->info_flags = 0;

    // snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
    // &snd_ksound_capture_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_ksound_capture_ops);

    // snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
    // 64
    // * 1024, 64 * 1024); snd_pcm_set_managed_buffer_all(pcm,
    // SNDRV_DMA_TYPE_VMALLOC, NULL, 64*1024, 64*1024);

    // NOTE: зарегистрировать карту
    err = snd_card_register(k_card->card);
    if (err < 0) {
        pr_info("failed to register sound card\n");
        err = -1;
        goto __error9;
    }

    pr_info("kernel ALSA sound module loaded successfully\n");
    return 0;

__error9:
    // TODO: освобождается через snd_card_free?
__error8:
    BUG_ON(k_card == NULL || k_card->card == NULL);
    snd_card_free(k_card->card);
__error7:
    BUG_ON(k_card == NULL);
    kfree(k_card);
    k_card = NULL;
__error6:
    platform_device_unregister(pdev);
__error5:
    device_destroy(my_class, dev_num);
__error4:
    class_destroy(my_class);
__error3:
    cdev_del(&my_cdev);
__error2:
    unregister_chrdev_region(dev_num, 1);
__error1:
    return err;
}

/*
 * Уничтожает модуль, освобождает выделенные ресурсы.
 */
static void __exit ksound_exit(void) {
    BUG_ON(k_card == NULL || k_card->card == NULL);
    BUG_ON(k_card == NULL);
    BUG_ON(pdev == NULL);

    atomic_set(&k_card->running, 0);

    // TODO: нужен ли snd_card_disconnect(k_card->card)?
    snd_card_free(k_card->card);
    kfree(k_card);

    platform_device_unregister(pdev);
    pdev = NULL;

    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);

    pr_info("kernel ALSA sound module unloaded\n");
}

module_init(ksound_init);
module_exit(ksound_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ivars Rozhleys");
MODULE_DESCRIPTION("Kernel ALSA Sound Driver");
