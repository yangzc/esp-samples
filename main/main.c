#include <string.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "stdio.h"
#include <esp_sntp.h>
#include <driver/gpio.h>
#include <esp_vfs_fat.h>
#include "audio_thread.h"
#include "audio_pipeline.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "i2s_stream.h"
#include "board.h"
#include "audio_element.h"
#include "sdmmc_cmd.h"
#include "format_wav.h"
#include "../build/config/sdkconfig.h"
#include "wav_encoder.h"
#include "raw_stream.h"

// 网络部分
static const char *TAG = "wifi station";
static EventGroupHandle_t s_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    ESP_LOGI(TAG, "Event base: %s, event id: %d", event_base, (int)event_id);
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief 初始化WiFi连接
 *
 * @param ssid WiFi的SSID
 * @param password WiFi的密码
 */
void setup_wifi(const char *ssid, const char *password) {
    // 创建事件组
    s_event_group = xEventGroupCreate();
    // 初始化WiFi
    ESP_ERROR_CHECK(esp_netif_init());
    // 创建默认的WiFi事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // 创建默认的WiFi网络接口
    esp_netif_create_default_wifi_sta();
    // 初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // 设置WiFi模式为STA模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // 注册WiFi事件处理函数
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // 设置WiFi的SSID和密码
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "your_ssid",
            .password = "your_password",
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    // 启动WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    // 等待WiFi连接成功
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE, 
        portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", wifi_config.sta.ssid);
    }
}

/** 
 * @brief 初始化SNTP（简单网络时间协议）客户端
 * 初始化完成后，系统将尝试与指定的NTP服务器同步时间。
*/
void setup_sntp(void) {
    printf("Initializing SNTP\n");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    // 设置SNTP服务器
    esp_sntp_setservername(0, "pool.ntp.org");
    // 初始化SNTP
    esp_sntp_init();
    // 同步时间
    // ESP_ERROR_CHECK(esp_sntp_sync());
}

// SD卡部分
#define SD_MOUNT_POINT "/sdcard"
#define SPI_DMA_CHAN        SPI_DMA_CH_AUTO
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;
void setup_sdcard(void) {
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << 13) | (1ULL << 12) | (1ULL << 11) | (1ULL << 10);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(13, 1);
    gpio_set_level(12, 1);
    gpio_set_level(11, 1);
    gpio_set_level(10, 1);

    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 8 * 1024
    };
    ESP_LOGI(TAG, "Initializing SD card");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = 11,
        .miso_io_num = 10,
        .sclk_io_num = 12,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = 13;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

}

static audio_pipeline_handle_t recorder;
static audio_element_handle_t audio_reader;
#define I2S_NUM         (I2S_NUM_0)
#define CONFIG_PCM_SAMPLE_RATE (8000)
#define CONFIG_PCM_DATA_LEN     320
#define AUDIO_I2S_BITS   32
#define BYTE_RATE           (CONFIG_PCM_SAMPLE_RATE * (AUDIO_I2S_BITS / 8)) * 1
void setup_audio(void) {
    // 初始化音频管道
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    recorder = audio_pipeline_init(&pipeline_cfg);
    if(!recorder) {
        ESP_LOGE(TAG, "Failed to create audio recorder");
        return;
    }

    // 初始化音频输入
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(0, CONFIG_PCM_SAMPLE_RATE, AUDIO_I2S_BITS, AUDIO_STREAM_READER);
    i2s_cfg.task_core     = 0;
    i2s_cfg.stack_in_ext  = true;
    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_TYPE_ONLY_LEFT);
    // i2s_cfg.out_rb_size  = 2 * 1024;
    i2s_cfg.stack_in_ext  = true;
    audio_element_handle_t i2s_audio_reader = i2s_stream_init(&i2s_cfg);
    audio_pipeline_register(recorder, i2s_audio_reader, "i2s");

    // 初始化WAV编码器
    wav_encoder_cfg_t wav_encoder_cfg = DEFAULT_WAV_ENCODER_CONFIG();
    wav_encoder_cfg.task_core = 1;
    audio_element_handle_t wav_encoder = wav_encoder_init(&wav_encoder_cfg);
    audio_pipeline_register(recorder, wav_encoder, "encoder");

    // 初始化文件输出
    // https://github.com/m5stack/uiflow-micropython/blob/04a6d8b968d2df007b3312a41125c1d65dccb29f/m5stack/cmodules/adf_module/audio_recorder.c#L418
    raw_stream_cfg_t raw_stream_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_stream_cfg.type = AUDIO_STREAM_WRITER;
    audio_element_handle_t raw_stream = raw_stream_init(&raw_stream_cfg);
    // audio_element_set_uri(raw_stream, SD_MOUNT_POINT"/record.wav");
    // 修改输出流的信息
    // audio_element_info_t out_stream_info;
    // audio_element_getinfo(raw_stream, &out_stream_info);
    // out_stream_info.sample_rates = CONFIG_PCM_SAMPLE_RATE;
    // out_stream_info.channels = 1;
    // out_stream_info.bits = AUDIO_I2S_BITS;
    // audio_element_setinfo(raw_stream, &out_stream_info);
    audio_pipeline_register(recorder, raw_stream, "file");
    audio_reader = raw_stream;
    // 设置超时
    audio_element_set_output_timeout(audio_reader, portMAX_DELAY);

    const char *link_tag[] = {"i2s", "encoder", "file"};
    audio_pipeline_link(recorder, link_tag, 3);
    // start the pipeline
    audio_pipeline_run(recorder);

    int ret = 0;
    uint8_t *audio_pcm_buf = heap_caps_malloc(CONFIG_PCM_DATA_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_pcm_buf) {
        printf("Failed to alloc audio buffer!\n");
        goto THREAD_END;
    }
    FILE *f = fopen(SD_MOUNT_POINT"/record.wav", "a");
    // 判断文件是否存在
    struct stat st;
    if (stat(SD_MOUNT_POINT"/record.wav", &st) == 0) {
        // Delete it if it exists
        unlink(SD_MOUNT_POINT"/record.wav");
    }

    // 保存10s的音频数据
    uint32_t save_bytes = BYTE_RATE * 10;
    size_t bytes_read = 0;
    while(bytes_read < save_bytes) {
        ret = raw_stream_read(raw_stream, (char *)audio_pcm_buf, CONFIG_PCM_DATA_LEN);
        if (ret != CONFIG_PCM_DATA_LEN) {
            printf("read raw stream error, expect %d, but only %d\n", CONFIG_PCM_DATA_LEN, ret);
        }
        // write to file
        fwrite(audio_pcm_buf, ret, 1, f);
        bytes_read += ret;
    }
    fclose(f);


    // uint8_t *audio_pcm_buf = heap_caps_malloc(CONFIG_PCM_DATA_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // if (!audio_pcm_buf) {
    //     printf("Failed to alloc audio buffer!\n");
    //     goto THREAD_END;
    // }
    // FILE *f = fopen(SD_MOUNT_POINT"/record.wav", "a");
    // if (f == NULL) {
    //     ESP_LOGE(TAG, "Failed to open file for writing");
    //     return;
    // }
    // 写入WAV文件头
    // uint32_t flash_rec_time = BYTE_RATE * 10;
    // const wav_header_t wav_header =
    //     WAV_HEADER_PCM_DEFAULT(flash_rec_time, AUDIO_I2S_BITS, CONFIG_PCM_SAMPLE_RATE, 1);
    // fwrite(&wav_header, sizeof(wav_header), 1, f);
    // // 读取音频数据
    // while(true) {
    //     int ret = raw_stream_read(audio_reader, audio_pcm_buf, CONFIG_PCM_DATA_LEN);
    //     if(ret != CONFIG_PCM_DATA_LEN) {
    //         printf("Failed to read audio data!\n");
    //     }
    //     fwrite(audio_pcm_buf, ret, 1, f);
    // }
    // fclose(f);

THREAD_END:
    if(audio_pcm_buf) {
        free(audio_pcm_buf);
    }
}

int app_main(void) {
    // 初始化非易失性存储
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();   
    }
    ESP_ERROR_CHECK(ret);
    // 初始化SD卡
    setup_sdcard();
    // 初始化WiFi连接
    setup_wifi("your_ssid", "your_password");
    // 初始化SNTP客户端(用于同步时间)
    setup_sntp();
    // 初始化音频采集
    setup_audio();
    return 0;
}
