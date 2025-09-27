#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/math64.h>
#include <linux/ktime.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define DRIVER_NAME "ksound"
#define CARD_NAME "KernelSoundCard"
#define BUFFER_SIZE (64 * 1024)

// speaker-test -D hw:1,0 -c 1 -t sine -r 48000 -f 400 | aplay -D hw:0,0 -r 48000 -f S16_LE -c 2 -B 100000 -v
// alsaloop -C hw:1,0 -P hw:0,0 -c 2 -f S16_LE -r 48000

static struct platform_device *pdev;
static struct ksound_card *card2;
static struct snd_pcm *pcm;

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
static struct snd_pcm_hardware snd_ksound_playback_hw = {
    .info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID),
    .formats = SNDRV_PCM_FMTBIT_S16_LE,
    .rates = SNDRV_PCM_RATE_48000,
    .rate_min = 48000,
    .rate_max = 48000,
    .channels_min = 1,              // на моеё системе вывод только 2 канала
    .channels_max = 1,
    .buffer_bytes_max = 64 * 1024,  //BUFFER_SIZE,
    .period_bytes_min = 1024,
    .period_bytes_max = 32 * 1024,  //BUFFER_SIZE,
    .periods_min = 2,
    .periods_max = 1024,
	.fifo_size = 0,
};

/*
 * обработка сэмплов буфера
 */
static enum hrtimer_restart ksound_timer_callback(struct hrtimer *timer)
{
    struct ksound_card *card = container_of(timer, struct ksound_card, timer);
    struct snd_pcm_substream *substream = card->substream;
    struct snd_pcm_runtime *runtime;
    s16 *samples;
    size_t period_bytes, offset_bytes;
    u64 period_ns;
    int i;

    printk(KERN_INFO "ksound: ksound_timer_callback\n");
    if (!atomic_read(&card->running) || !substream)
        return HRTIMER_NORESTART;

    runtime = substream->runtime;
    samples = (s16*)runtime->dma_area;
    period_bytes = frames_to_bytes(runtime, runtime->period_size);

    offset_bytes = frames_to_bytes(runtime, card->hw_ptr) % runtime->dma_bytes;
    s16 *ptr = (s16*)((char*)samples + offset_bytes);
    
    for (i = 0; i < runtime->period_size; i++) {
    	//printk("%d ", ptr[i]);
    }
    
    // подвинуть указатель
    card->hw_ptr = (card->hw_ptr + runtime->period_size) % runtime->buffer_size;

    // уведомить ALSA
    snd_pcm_period_elapsed(substream);

    // посчитать продолжительность периода в нс
    period_ns = div_u64((u64)runtime->period_size * NSEC_PER_SEC, runtime->rate);
    
    // задать таймер снова
    hrtimer_forward_now(timer, ns_to_ktime(period_ns));
    return HRTIMER_RESTART;
}

/*
 * открыть PCM поток
 */
static int snd_ksound_playback_open(struct snd_pcm_substream *substream)
{
    struct ksound_card *card = substream->pcm->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
        
    printk(KERN_INFO "ksound: snd_ksound_playback_open\n");

    substream->private_data = card;
    runtime->hw = snd_ksound_playback_hw;
    card->substream = substream;
       
    return 0;
}

/*
 * закрыть PCM поток
 */
static int snd_ksound_playback_close(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct ksound_card *card = substream->private_data;
        
    card->substream = NULL;
    substream->private_data = NULL;
    
    printk(KERN_INFO "ksound: snd_ksound_playback_close\n");
    return 0;
}

/*
 * указатель на место проигрывания в буфере
 */
static snd_pcm_uframes_t snd_ksound_playback_pointer(struct snd_pcm_substream *substream)
{
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
    printk(KERN_INFO "ksound: %lu, %lu, %p",
    		card->hw_ptr, runtime->period_size, runtime->dma_area); 
    return card->hw_ptr;
}

/*
 * почему этот метод магическим образом очищает поток?
 */
/*static snd_pcm_uframes_t snd_ksound_playback_pointer(struct snd_pcm_substream *substream)
{
	struct ksound_card *card = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	static snd_pcm_uframes_t simulated_position = 0;

	if (atomic_read(&card->running)) 
	{		
		// Advance the simulated position 
		simulated_position += runtime->period_size; 
//		printk(KERN_INFO "ksound: %lu, %lu, %p",
//				simulated_position, runtime->period_size, runtime->dma_area); 
		
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
static int snd_ksound_playback_prepare(struct snd_pcm_substream *substream)
{
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
   
    int buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size); // params_buffer_bytes(hw_params)
    
    s16 *samples = (s16*)runtime->dma_area;    
    printk(KERN_INFO "ksound: snd_ksound_playback_prepare buffer_size=%ld, period_size=%ld, format=%d, buffer_bytes=%d\n",
    		runtime->buffer_size, runtime->period_size, runtime->format, buffer_bytes);
    
    card->buffer_size = runtime->buffer_size;
    card->period_size = runtime->period_size;
    
    return 0;
}

/*
 * смена состояние PCM
 */
static int snd_ksound_playback_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
    
    printk(KERN_INFO "ksound: snd_ksound_playback_trigger cmd=%d, dma=%p\n",
        		cmd, runtime->dma_area);
        
    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
    {    	
        atomic_set(&card->running, 1);
        card->hw_ptr = 0;
        card->frames_since_start = 0;
        card->last_time_ns = ktime_get_ns();
        
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

static int snd_ksound_playback_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
    struct ksound_card *card = substream->private_data;
    struct snd_pcm_runtime *runtime = substream->runtime;
    int err = 0, buffer_bytes = 0;
        
    // создать буфер
    err = snd_pcm_lib_alloc_vmalloc_buffer(substream, params_buffer_bytes(hw_params));
    if (err < 0) {
        printk(KERN_ERR "ksound: snd_ksound_playback_hw_params Failed to allocate buffer\n");
        return err;
    }

    buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
    card->buffer_size = params_buffer_size(hw_params);
    card->period_size = params_period_size(hw_params);
        
    printk(KERN_INFO "ksound: snd_ksound_playback_hw_params buffer_size=%lu, period_size=%lu, buffer_bytes=%u\n",
           card->buffer_size, card->period_size, buffer_bytes);
    
    return 0;
}

/*
 * разрешённые операции с PCM
 */
static struct snd_pcm_ops snd_ksound_playback_ops = {
    .open = snd_ksound_playback_open,
    .close = snd_ksound_playback_close,
    .ioctl = snd_pcm_lib_ioctl,
	.hw_params = snd_ksound_playback_hw_params,
    .hw_free = snd_pcm_lib_free_vmalloc_buffer,
    .prepare = snd_ksound_playback_prepare,
    .trigger = snd_ksound_playback_trigger,
    .pointer = snd_ksound_playback_pointer,
	.page = snd_pcm_lib_get_vmalloc_page,  // Use this for vmalloc buffers
};

/*
 * Создать PCM устройство
 */
static int snd_ksound_new_pcm(struct snd_card *card)
{
    int err;

    err = snd_pcm_new(card, DRIVER_NAME, 0, 1, 0, &pcm); // 1 playback, 0 capture
    if (err < 0)
        return err;

    strcpy(pcm->name, CARD_NAME);
    pcm->private_data = card;
    pcm->info_flags = 0;

    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_ksound_playback_ops);
    
    //snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 64 * 1024, 64 * 1024);
	//snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 64*1024, 64*1024);

    return 0;
}

/*
 * инициализация модуля
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
    card2 = kzalloc(sizeof(*card2), GFP_KERNEL);
    if (!card2) {
        printk(KERN_ERR "ksound: Cannot allocate card\n");
        return -ENOMEM;
    }
        
    // инциализация полей структуры карты
    atomic_set(&card2->running, 0);
    card2->hw_ptr = 0;
    card2->substream = NULL;
    
    // создать ALSA карту, в качестве родителя драйвер платформы
    // aplay -l
    err = snd_card_new(&pdev->dev, -1, DRIVER_NAME, THIS_MODULE, 0, &card2->card);
    if (err < 0) {
        pr_err("Cannot create sound card\n");
        goto error1;
    }

    strcpy(card2->card->driver, DRIVER_NAME);
    strcpy(card2->card->shortname, CARD_NAME);
    sprintf(card2->card->longname, "%s at virtual", CARD_NAME);

    // создать pcm устройство
    err = snd_ksound_new_pcm(card2->card);
    if (err < 0)
        goto error2;

    // зарегистрировать карту
    err = snd_card_register(card2->card);
    if (err < 0)
        goto error2;

    pr_info("Kernel ALSA sound module loaded successfully\n");
    return 0;

error2:
    snd_card_free(card2->card);
error1:
    platform_device_unregister(pdev);
    return err;
}

/*
 * деструктор модуля
 */
static void __exit ksound_exit(void)
{
    if (card2)
    {
        snd_card_free(card2->card);
    	//del_timer_sync(&card2->timer);
	}
    
    if (pdev)
    {
        platform_device_unregister(pdev);
    }
    
    kfree(card2);
    pr_info("Kernel ALSA sound module unloaded\n");
}

module_init(ksound_init);
module_exit(ksound_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ivars Rozhleys");
MODULE_DESCRIPTION("Kernel ALSA Sound Driver");
