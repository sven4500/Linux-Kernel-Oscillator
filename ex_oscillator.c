#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/math64.h>
#include <linux/ktime.h>
#include <linux/types.h>    // s16, u64, size_t, ...
#include <sound/asound.h>   // snd_pcm_uframes_t, ...
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define DRIVER_NAME "ksound"
#define CARD_NAME "KernelSoundCard"
#define BUFFER_SIZE (64 * 1024)

// speaker-test -D hw:1,0 -c 1 -t sine -r 48000 -f 400 | aplay -D hw:0,0 -r 48000 -f S16_LE -c 2 -B 100000 -v
// alsaloop -C hw:1,0 -P hw:0,0 -c 2 -f S16_LE -r 48000

/*
 * Описание виртуальной карты
 */
struct ksound_card
{
    struct snd_card *card;
    struct snd_pcm *pcm;
    struct hrtimer timer;
    struct snd_pcm_substream *substream;
    atomic_t running;
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;
    snd_pcm_uframes_t hw_ptr;               // указатель проигрываемое место в бфере
    u64 last_time_ns;                       // время обновления указателя
    u64 frames_since_start;                 // всего кадров с начала запуска
};

/*
 * Описвание устройства PCM
 */
static struct snd_pcm_hardware snd_ksound_capture_hw = {
    .info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID),
    .formats = SNDRV_PCM_FMTBIT_S16_LE,
    .rates = SNDRV_PCM_RATE_48000,
    .rate_min = 48000,              // минимальная частота дискретизации (runtime->rate)
    .rate_max = 48000,              // максимальная частота дискретизации (runtime->rate)
    .channels_min = 2,              // минимальное и максимальное количество каналов
    .channels_max = 2,              // на моей системе вывод динамиков только в 2 канала
    .buffer_bytes_max = 128 * 1024, //BUFFER_SIZE,
    .period_bytes_min = 256,
    .period_bytes_max = 16 * 1024,  //BUFFER_SIZE,
    .periods_min = 4,
    .periods_max = 1024,
};

/*
 * Генерирует пилообразный сигнал.
 */
static void make_saw_ramp(s16* samples, size_t count, int rate, int frequency, int phase)
{
    size_t i;
    for (i = 0; i < count; i++) {
        // Saw ramp: scale phase to 16-bit range
        s16 sample = (s16)(((phase * 8192) / rate) - 4096); // 65536, 32768, 16384, 8192, 4096

        // Write stereo (L+R same)
        samples[i * 2 + 0] = sample;
        samples[i * 2 + 1] = sample;

        // Advance phase by frequency (e.g., 400 Hz)
        phase += frequency;

        if (phase >= rate) {
            phase -= rate;
        }
    }
}

/*
 * Обработка сэмплов буфера. runtime->rate частота дискретизации канала.
 */
static enum hrtimer_restart ksound_timer_callback(struct hrtimer *timer)
{
    struct ksound_card *const card = container_of(timer, struct ksound_card, timer);
    struct snd_pcm_substream *const substream = card->substream;
    struct snd_pcm_runtime *const runtime = substream->runtime;

    // period - аудио фрагмент, frames - количество дискрет на фрагмент. У нас
    // 2 канала и 16 бит на канал поэтому frames_to_bytes вернёт period * 4.
    s16 *const samples = (s16*)(runtime->dma_area + card->hw_ptr);
    size_t const period_bytes = frames_to_bytes(runtime, runtime->period_size);
    size_t const buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
    u64 period_ns;
    ktime_t const now = ktime_get();

    // runtime->dma_bytes - размер DMA области в байтах, заметил что DMA область
    // может быть чуть больше чем размер буфера
    BUG_ON(runtime->dma_bytes == 0);
    BUG_ON(substream == 0);
    BUG_ON(card->hw_ptr >= runtime->dma_bytes);
    WARN_ON(buffer_bytes > runtime->dma_bytes);

    //printk(KERN_INFO "ksound: ksound_timer_callback hw_ptr=%lu, period=%lu, buffer=%lu, dmabytes=%lu",
           //card->hw_ptr, runtime->period_size, runtime->buffer_size, runtime->dma_bytes);

    if (!atomic_read(&card->running))
        return HRTIMER_NORESTART;

    // проверить что не выходим за область DMA, если выйти возможно падение ядра
    if (runtime->dma_bytes - card->hw_ptr >= period_bytes)
        make_saw_ramp(samples, runtime->period_size, runtime->rate, 400, 0);

    //for (i = 0; i < runtime->period_size; i++)
        //printk("%d ", ptr[i]);

    // Подвинуть указатель на следующий фрагмент. Лучше переходить в начало или
    // с сохранением хвоста?
    // card->hw_ptr = (card->hw_ptr + period_bytes) % runtime->dma_bytes;
    card->hw_ptr += period_bytes;
    if (card->hw_ptr >= runtime->dma_bytes)
        card->hw_ptr = 0;

    // уведомить ALSA
    snd_pcm_period_elapsed(substream);

    // Продолжительность периода в нс. Количество дискрет поделить на частоту
    // дискретизации даёт секунды, умножаем на NSEC_PER_SEC чтобы получить нс.
    period_ns = div_u64(runtime->period_size * NSEC_PER_SEC, runtime->rate);

    // Задать таймер, так тоже можно. Пока не понимаю как лучше
    //hrtimer_forward_now(timer, ns_to_ktime(period_ns))
    hrtimer_forward(timer, now, ns_to_ktime(period_ns));
    return HRTIMER_RESTART;
}

/*
 * открыть PCM поток
 */
static int snd_ksound_capture_open(struct snd_pcm_substream *substream)
{
    struct ksound_card *card = substream->pcm->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;

    card->substream = substream;
    substream->private_data = card;
    runtime->hw = snd_ksound_capture_hw;

    printk(KERN_INFO "ksound: snd_ksound_capture_open\n");
    return 0;
}

/*
 * закрыть PCM поток
 */
static int snd_ksound_capture_close(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct ksound_card *card = substream->private_data;
        
    card->substream = NULL;
    substream->private_data = NULL;
    
    printk(KERN_INFO "ksound: snd_ksound_capture_close\n");
    return 0;
}

/*
 * Указатель на место проигрывания в буфере. Возвращает указатель в дискретах.
 */
static snd_pcm_uframes_t snd_ksound_capture_pointer(struct snd_pcm_substream *substream)
{
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
    //printk(KERN_INFO "ksound: hw_ptr=%lu, period=%lu, bytes=%d\n",
           //card->hw_ptr, runtime->period_size, bytes_to_frames(substream->runtime, card->hw_ptr));

    // похоже что ALSA подсистеме нужен указатель в дискретах, а не байтах
    return bytes_to_frames(substream->runtime, card->hw_ptr);
}

/*
 * почему этот метод магическим образом очищает поток?
 */
/*static snd_pcm_uframes_t snd_ksound_capture_pointer(struct snd_pcm_substream *substream)
{
	struct ksound_card *card = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	static snd_pcm_uframes_t simulated_position = 0;

	if (atomic_read(&card->running)) 
	{		
		// Advance the simulated position 
		simulated_position += runtime->period_size; 
        //printk(KERN_INFO "ksound: %lu, %lu, %p", simulated_position, runtime->period_size, runtime->dma_area);
		
		// Wrap around at buffer size 
		if (simulated_position >= runtime->buffer_size)
			simulated_position = 0;

		// Update the card's hardware pointer 
		card->hw_ptr = simulated_position; 
	}
	
	return card->hw_ptr; 
}*/

/*
 * подготовка PCM потока
 */
static int snd_ksound_capture_prepare(struct snd_pcm_substream *substream)
{
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
   
    int buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size); // params_buffer_bytes(hw_params)

    printk(KERN_INFO "ksound: snd_ksound_capture_prepare buffer_size=%ld, period_size=%ld, format=%d, buffer_bytes=%d\n",
           runtime->buffer_size, runtime->period_size, runtime->format, buffer_bytes);
    
    card->buffer_size = runtime->buffer_size;
    card->period_size = runtime->period_size;
    
    //make_saw_ramp((s16*)runtime->dma_area, runtime->buffer_size, runtime->rate, 400, 0);
    return 0;
}

/*
 * смена состояние PCM
 */
static int snd_ksound_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
    
    printk(KERN_INFO "ksound: snd_ksound_capture_trigger cmd=%d, dma=%p\n", cmd, runtime->dma_area);
        
    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
    {
        card->hw_ptr = 0;
        card->frames_since_start = 0;
        card->last_time_ns = ktime_get_ns();

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
        
    default:
        return -EINVAL;
    }
}

static int snd_ksound_capture_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
    int err = 0, buffer_bytes = 0;
        
    // создать буфер
    err = snd_pcm_lib_alloc_vmalloc_buffer(substream, params_buffer_bytes(hw_params));
    if (err < 0) {
        printk(KERN_ERR "ksound: snd_ksound_capture_hw_params Failed to allocate buffer\n");
        return err;
    }

    buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
    card->buffer_size = params_buffer_size(hw_params);
    card->period_size = params_period_size(hw_params);
        
    printk(KERN_INFO "ksound: snd_ksound_capture_hw_params buffer_size=%lu, period_size=%lu, buffer_bytes=%u\n",
           card->buffer_size, card->period_size, buffer_bytes);
    
    return 0;
}

/*
 * разрешённые операции с PCM
 */
static struct snd_pcm_ops snd_ksound_capture_ops = {
    .open = snd_ksound_capture_open,
    .close = snd_ksound_capture_close,
    .ioctl = snd_pcm_lib_ioctl,
	.hw_params = snd_ksound_capture_hw_params,
    .hw_free = snd_pcm_lib_free_vmalloc_buffer,
    .prepare = snd_ksound_capture_prepare,
    .trigger = snd_ksound_capture_trigger,
    .pointer = snd_ksound_capture_pointer,
	//.page = snd_pcm_lib_get_vmalloc_page,  // Use this for vmalloc buffers
};

/*
 * Глобальные переменные для хранения дескрипторов устройства, карты и пр. Эти
 * значения зашиваются в поля private_data карты и pcm потока и функциям следует
 * брать эти данные оттуда поэтому эти переменные максимально внизу.
 */
static struct platform_device *pdev;
static struct snd_pcm *pcm;
static struct ksound_card *k_card;

/*
 * Инициализирует модуль. Создаёт новый драйвер платформы который выступает в
 * качестве родителя для ALSA карты.
 */
static int __init ksound_init(void)
{
    int err;

    // создать драйвер платформы
    pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
    if (IS_ERR(pdev)) {
        pr_err("Cannot create platform device\n");
        return PTR_ERR(pdev);
    }
    
    // создать виртуальную карту
    k_card = kzalloc(sizeof(*k_card), GFP_KERNEL);
    if (!k_card) {
        printk(KERN_ERR "ksound: Cannot allocate card\n");
        return -ENOMEM;
    }
        
    // инциализация полей структуры карты
    atomic_set(&k_card->running, 0);
    k_card->hw_ptr = 0;
    
    // создать ALSA карту, в качестве родителя драйвер платформы (aplay -l)
    err = snd_card_new(&pdev->dev, -1, DRIVER_NAME, THIS_MODULE, 0, &k_card->card);
    if (err < 0) {
        pr_err("Cannot create sound card\n");
        goto __error1;
    }

    strcpy(k_card->card->driver, DRIVER_NAME);
    strcpy(k_card->card->shortname, CARD_NAME);
    sprintf(k_card->card->longname, "%s at virtual", CARD_NAME);

    // создать pcm устройство
    err = snd_pcm_new(k_card->card, DRIVER_NAME, 0, 0, 1, &pcm); // playback, capture
    if (err < 0)
        goto __error2;

    strcpy(pcm->name, CARD_NAME);
    pcm->private_data = k_card;
    pcm->info_flags = 0;

    //snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_ksound_capture_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_ksound_capture_ops);

    //snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 64 * 1024, 64 * 1024);
    //snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 64*1024, 64*1024);

    // зарегистрировать карту
    err = snd_card_register(k_card->card);
    if (err < 0)
        goto __error2;

    pr_info("Kernel ALSA sound module loaded successfully\n");
    return 0;

__error2:
    snd_card_free(k_card->card);
__error1:
    platform_device_unregister(pdev);
    return err;
}

/*
 * деструктор модуля
 */
static void __exit ksound_exit(void)
{
    if (k_card)
    {
        snd_card_free(k_card->card);
	}
    
    if (pdev)
    {
        platform_device_unregister(pdev);
    }
    
    kfree(k_card);
    pr_info("Kernel ALSA sound module unloaded\n");
}

module_init(ksound_init);
module_exit(ksound_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ivars Rozhleys");
MODULE_DESCRIPTION("Kernel ALSA Sound Driver");
