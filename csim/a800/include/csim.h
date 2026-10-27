#ifndef A8BB_CSIM_H
#define A8BB_CSIM_H

#include <stddef.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <climits>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <algorithm>
#include <string>
using std::string;
class Csim_Module { public: virtual ~Csim_Module() = default; virtual void parseArg(char **&, char **) {} virtual void loop() {} };
class HTTPClient { public: template<class Callback> static void csim_onPOST(const char *, Callback) {} };
static inline size_t csim_strlcpy(char *dst, const char *src, size_t size) { size_t n = strlen(src); if (size) { size_t c = n < size - 1 ? n : size - 1; memcpy(dst, src, c); dst[c] = 0; } return n; }
#define strlcpy csim_strlcpy

#undef CSIM_HOOK
#ifdef ATARI800_SIM_RUNNER
extern void csim_hook();
#define CSIM_HOOK() csim_hook()
#else
#define CSIM_HOOK() do {} while (0)
#endif

#define IRAM_ATTR
#define DRAM_ATTR
#define FLASH_ATTR
#define IFLASH_ATTR
#define DRAM_STR(value) value
#define RTC_DATA_ATTR
#define __NOINIT_ATTR
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define portENTER_CRITICAL_ISR(mux) ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux) ((void)(mux))
#define portDISABLE_INTERRUPTS() ((void)0)
#define portENABLE_INTERRUPTS() ((void)0)
#define portYIELD_FROM_ISR() ((void)0)
#define portYIELD_FROM_ISR_IF(should_yield) ((void)(should_yield))
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY UINT32_MAX
#define CONFIG_FREERTOS_UNICORE 1
#define CONFIG_ESP_INT_WDT 0
#define CONFIG_A8BB_ENABLE_MDNS 0
#define CONFIG_SMB_NEED_PASSWORD 0
#define GIT_VERSION "csim"
#define BIT0 (1U << 0)
#define BIT1 (1U << 1)
#define BIT2 (1U << 2)
#define BIT3 (1U << 3)
#define BIT4 (1U << 4)
#define BIT5 (1U << 5)
#define BIT6 (1U << 6)
#define BIT7 (1U << 7)
#define BIT8 (1U << 8)
#define BIT9 (1U << 9)
#define BIT10 (1U << 10)
#define BIT11 (1U << 11)
#define BIT12 (1U << 12)
#define BIT13 (1U << 13)
#define BIT14 (1U << 14)
#define BIT15 (1U << 15)
#define BIT16 (1U << 16)
#define BIT17 (1U << 17)
#define BIT18 (1U << 18)
#define BIT19 (1U << 19)
#define BIT20 (1U << 20)
#define BIT21 (1U << 21)
#define BIT22 (1U << 22)
#define BIT23 (1U << 23)
#define BIT24 (1U << 24)
#define BIT25 (1U << 25)
#define BIT26 (1U << 26)
#define BIT27 (1U << 27)
#define BIT28 (1U << 28)
#define BIT29 (1U << 29)
#define BIT30 (1U << 30)
#define BIT31 (1U << 31)

typedef int esp_err_t;
typedef int gpio_num_t;
typedef int gpio_mode_t;
typedef int gpio_int_type_t;
typedef int uart_port_t;
typedef int uart_config_t;
typedef int spi_host_device_t;
typedef int TaskHandle_t;
typedef int SemaphoreHandle_t;
typedef int EventGroupHandle_t;
typedef int esp_timer_handle_t;
typedef struct spiffs_t spiffs_t;
typedef spiffs_t spiffs;
typedef int spiffs_file;
typedef struct { size_t size; } spiffs_stat;
typedef struct { int unused; } spiffs_DIR;
typedef struct { unsigned char name[256]; } spiffs_dirent;
int SPIFFS_stat(spiffs_t *fs, const char *path, spiffs_stat *stat);
spiffs_file SPIFFS_open(spiffs_t *fs, const char *path, int flags, int mode);
int SPIFFS_close(spiffs_t *fs, spiffs_file fd);
int SPIFFS_read(spiffs_t *fs, spiffs_file fd, void *buf, int size);
int SPIFFS_write(spiffs_t *fs, spiffs_file fd, const void *buf, int size);
int SPIFFS_lseek(spiffs_t *fs, spiffs_file fd, int offset, int whence);
spiffs_DIR *SPIFFS_opendir(spiffs_t *fs, const char *path, spiffs_DIR *dir);
spiffs_dirent *SPIFFS_readdir(spiffs_DIR *dir, spiffs_dirent *entry);
void SPIFFS_closedir(spiffs_DIR *dir);
typedef int gpio_pullup_t;
typedef int gpio_pulldown_t;
typedef int gpio_int_type_t;
typedef int ledc_mode_t;
typedef int ledc_channel_t;
typedef int ledc_timer_t;
typedef int ledc_timer_bit_t;
typedef int ledc_clk_cfg_t;
typedef int UBaseType_t;
typedef uint32_t EventBits_t;
typedef void *rmt_channel_handle_t;
typedef void *rmt_encoder_handle_t;
typedef struct { int gpio_num; int clk_src; uint32_t resolution_hz; size_t mem_block_symbols; size_t trans_queue_depth; } rmt_tx_channel_config_t;
typedef struct { int unused; } rmt_copy_encoder_config_t;
typedef struct { uint32_t duration0, duration1, level0, level1; } rmt_symbol_word_t;
typedef struct { int loop_count; } rmt_transmit_config_t;
typedef struct { int out_en; } dedic_gpio_bundle_flags_t;
typedef struct { int *gpio_array; size_t array_size; dedic_gpio_bundle_flags_t flags; } dedic_gpio_bundle_config_t;
typedef void *dedic_gpio_bundle_handle_t;
typedef void *esp_event_handler_instance_t;
typedef const char *esp_event_base_t;
typedef uint32_t BaseType_t;
typedef struct { uint8_t ssid[32]; uint8_t password[64]; } wifi_sta_config_t;
typedef struct { wifi_sta_config_t sta; } wifi_config_t;
typedef struct { int unused; } wifi_init_config_t;
typedef struct { uint32_t ip; } ip_event_got_ip_t;
typedef void *httpd_handle_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(void *); void *user_ctx; } httpd_uri_t;
typedef struct { int lru_purge_enable; } httpd_config_t;
typedef struct { void *user_ctx; } httpd_req_t;
typedef struct { const char *base_path; const char *partition_label; int max_files; bool format_if_mount_failed; } esp_vfs_spiffs_conf_t;
typedef struct { int tx_buffer_size; int rx_buffer_size; } usb_serial_jtag_driver_config_t;
typedef struct { void *ctx; void *on_client_connected; void *on_client_disconnected; void *on_baudrate; void *on_control; void *on_purge; void *on_data_received; uint16_t port; int task_stack_size; int task_priority; int task_core_id; } rfc2217_server_config_t;
typedef uint32_t StackType_t;
typedef void *rfc2217_server_t;

typedef struct {
	uint64_t pin_bit_mask;
	gpio_mode_t mode;
	gpio_pullup_t pull_up_en;
	gpio_pulldown_t pull_down_en;
	gpio_int_type_t intr_type;
} gpio_config_t;

typedef struct {
	ledc_mode_t speed_mode;
	ledc_timer_bit_t duty_resolution;
	ledc_timer_t timer_num;
	uint32_t freq_hz;
	ledc_clk_cfg_t clk_cfg;
} ledc_timer_config_t;

typedef struct {
	ledc_channel_t channel;
	int gpio_num;
	uint32_t duty;
	ledc_mode_t speed_mode;
	ledc_timer_t timer_sel;
} ledc_channel_config_t;

typedef struct { int int_type; } csim_gpio_pin_t;
typedef struct { csim_gpio_pin_t pin[64]; } csim_gpio_dev_t;
typedef struct { csim_gpio_dev_t *dev; } gpio_hal_context_t;
static inline csim_gpio_dev_t *csim_gpio_hw(void) { static csim_gpio_dev_t dev = {}; return &dev; }

#define GPIO_IN_REG 0
#define GPIO_IN1_REG 1
#define GPIO_OUT_REG 2
#define GPIO_OUT1_REG 3
#define GPIO_PORT_0 0
#define REG_READ(address) (0U)
#define REG_WRITE(address, value) ((void)0)
#define REG_SET_BIT(address, mask) ((void)0)
#define REG_CLR_BIT(address, mask) ((void)0)
#define REG_GET_BIT(address, mask) (0U)
#define DPORT_REG_WRITE(address, value) ((void)0)
#define DPORT_REG_SET_BIT(address, mask) ((void)0)
#define DPORT_REG_CLR_BIT(address, mask) ((void)0)
#define GPIO_MODE_DISABLE 0
#define GPIO_MODE_DEF_OD 4
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_INTR_DISABLE 0
#define GPIO_LL_GET_HW(port) (csim_gpio_hw())
#define GPIO_IS_VALID_GPIO(pin) (1)
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) (1)
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_AUTO_CLK 0
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_DMA 0
#define MALLOC_CAP_8BIT 0
#define SPIFFS_O_TRUNC O_TRUNC
#define SPIFFS_O_RDONLY O_RDONLY
#define SPIFFS_O_RDWR O_RDWR
#define SPIFFS_O_CREAT O_CREAT
#define SPIFFS_SEEK_SET SEEK_SET
#define RMT_CLK_SRC_DEFAULT 0
#define HTTP_GET 0
#define ESP_MAC_WIFI_STA 0
#define ESP_EVENT_ANY_ID (-1)
#define WIFI_EVENT "WIFI_EVENT"
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT "IP_EVENT"
#define IP_EVENT_STA_GOT_IP 1
#define WIFI_MODE_STA 0
#define WIFI_IF_STA 0
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_TASK_MAIN_STACK 4096
#define ESP_TASKD_EVENT_STACK 4096
#define ESP_TASK_TIMER_STACK 4096
#define ESP_TASK_TCPIP_STACK 4096
#define CONFIG_ESP_IPC_TASK_STACK_SIZE 4096
#define CONFIG_FREERTOS_TIMER_SERVICE_TASK_NAME "timer"
#define CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH 4096
#define CONFIG_ESP32_PTHREAD_TASK_NAME_DEFAULT "pthread"
#define CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT 4096
#define CORE1_GPIO_IN0_IDX 0
#define SYSTEM_CORE_1_CONTROL_0_REG 0
#define SYSTEM_CORE_1_CONTROL_1_REG 0
#define SYSTEM_CONTROL_CORE_1_CLKGATE_EN 1
#define SYSTEM_CONTROL_CORE_1_RESETING 2
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) 0, 0, 0, 0

#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x102
#define HTTPD_RESP_USE_STRLEN (-1)
#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() ((usb_serial_jtag_driver_config_t){0,0})
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define ESP_RETURN_ON_FALSE(cond, err, tag, format, ...) do { if (!(cond)) return (err); } while (0)
#define ESP_ERROR_CHECK(expression) ((void)(expression))
#define ESP_LOGE(tag, format, ...) ((void)0)
#define ESP_LOGW(tag, format, ...) ((void)0)
#define ESP_LOGI(tag, format, ...) ((void)0)
#define ESP_LOGD(tag, format, ...) ((void)0)
#define ESP_LOGV(tag, format, ...) ((void)0)
#define ESP_RETURN_ON_ERROR(expression, tag, format, ...) do { if ((expression) != ESP_OK) return (expression); } while (0)
#define ESP_GOTO_ON_ERROR(expression, label, tag, format, ...) do { if ((expression) != ESP_OK) goto label; } while (0)

static inline uint64_t &CSIM_CCOUNT_VALUE(void) {
    static uint64_t ccount = 0;
    return ccount;
}
static inline uint32_t XTHAL_GET_CCOUNT(void) {
    return static_cast<uint32_t>(++CSIM_CCOUNT_VALUE());
}
static inline uint32_t CSIM_PEEK_CCOUNT(void) {
    return static_cast<uint32_t>(CSIM_CCOUNT_VALUE());
}
static inline uint64_t CSIM_PEEK_CCOUNT64(void) {
    return CSIM_CCOUNT_VALUE();
}
static inline void CSIM_ADVANCE_CCOUNT(uint32_t ticks) {
    CSIM_CCOUNT_VALUE() += ticks;
}
static inline uint32_t esp_timer_get_time(void) { return 0; }
extern uint8_t *atariRam;
#ifdef ATARI800_SIM
extern "C" uint8_t pbiROM[0x800];
#else
extern uint8_t pbiROM[0x800];
#endif
static inline void vTaskDelay(uint32_t ticks) { (void)ticks; }
static inline BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *, BaseType_t) { return pdPASS; }
static inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return 0; }
static inline TaskHandle_t xTaskGetHandle(const char *) { return 0; }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return 0; }
static inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return pdFALSE; }
static inline int xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
static inline EventGroupHandle_t xEventGroupCreate(void) { return 0; }
static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t bits) { return bits; }
static inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t bits, int, int, uint32_t) { return bits; }
static inline esp_err_t esp_read_mac(uint8_t *mac, int) {
    static const uint8_t csim_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(mac, csim_mac, sizeof(csim_mac));
    return ESP_OK;
}
static inline const char *esp_err_to_name(esp_err_t) { return "CSIM"; }
static inline esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *) { return ESP_OK; }
static inline esp_err_t esp_spiffs_info(const char *, size_t *total, size_t *used) { if(total) *total=0; if(used) *used=0; return ESP_OK; }
static inline esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *) { return ESP_OK; }
static inline int usb_serial_jtag_read_bytes(void *, size_t, uint32_t) { return 0; }
static inline esp_err_t gpio_matrix_in(int, int, bool) { return ESP_OK; }
static inline void *heap_caps_aligned_alloc(size_t, size_t size, unsigned int) { return malloc(size); }
static inline esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *, rmt_channel_handle_t *out) { *out=nullptr; return ESP_OK; }
static inline esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *, rmt_encoder_handle_t *out) { *out=nullptr; return ESP_OK; }
static inline esp_err_t rmt_enable(rmt_channel_handle_t) { return ESP_OK; }
static inline esp_err_t rmt_transmit(rmt_channel_handle_t, rmt_encoder_handle_t, const void *, size_t, const rmt_transmit_config_t *) { return ESP_OK; }
static inline esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t, uint32_t) { return ESP_OK; }
static inline esp_err_t dedic_gpio_new_bundle(const dedic_gpio_bundle_config_t *, dedic_gpio_bundle_handle_t *out) { *out=nullptr; return ESP_OK; }
static inline void dedic_gpio_cpu_ll_write_all(uint32_t) {}
template<class Fn> static inline esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t, Fn, void *, esp_event_handler_instance_t *) { return ESP_OK; }
static inline esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
static inline esp_err_t esp_netif_init(void) { return ESP_OK; }
static inline void *esp_netif_create_default_wifi_sta(void) { return nullptr; }
static inline wifi_init_config_t WIFI_INIT_CONFIG_DEFAULT(void) { return {}; }
static inline esp_err_t esp_wifi_init(const wifi_init_config_t *) { return ESP_OK; }
static inline esp_err_t esp_wifi_connect(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_mode(int) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(int, wifi_config_t *) { return ESP_OK; }
static inline esp_err_t esp_wifi_start(void) { return ESP_OK; }
static inline esp_err_t nvs_flash_init(void) { return ESP_OK; }
static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }
static inline esp_err_t httpd_start(httpd_handle_t *, httpd_config_t *) { return ESP_OK; }
static inline esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *) { return ESP_OK; }
static inline size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *) { return 0; }
static inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t) { return ESP_OK; }
static inline size_t httpd_req_get_url_query_len(httpd_req_t *) { return 0; }
static inline esp_err_t httpd_req_get_url_query_str(httpd_req_t *, char *, size_t) { return ESP_OK; }
static inline esp_err_t httpd_query_key_value(const char *, const char *, char *, size_t) { return ESP_FAIL; }
static inline esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) { return ESP_OK; }
static inline esp_err_t httpd_resp_send(httpd_req_t *, const char *, ssize_t) { return ESP_OK; }
static inline esp_err_t ets_set_appcpu_boot_addr(uint32_t) { return ESP_OK; }
static inline esp_err_t rfc2217_server_create(const rfc2217_server_config_t *, rfc2217_server_t *) { return ESP_OK; }
static inline esp_err_t rfc2217_server_start(rfc2217_server_t) { return ESP_OK; }
static inline esp_err_t rfc2217_server_send_data(rfc2217_server_t, const uint8_t *, size_t) { return ESP_OK; }
static inline esp_err_t rfc2217_server_disconnect_client(rfc2217_server_t) { return ESP_OK; }
static inline esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t, void (*)(void *, esp_event_base_t, int32_t, void *), void *, esp_event_handler_instance_t *) { return ESP_OK; }
static inline int gpio_set_level(gpio_num_t pin, unsigned int level) { (void)pin; (void)level; return ESP_OK; }
static inline int gpio_get_level(gpio_num_t pin) { (void)pin; return 0; }
static inline esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
static inline esp_err_t ledc_timer_config(const ledc_timer_config_t *config) { (void)config; return ESP_OK; }
static inline esp_err_t ledc_channel_config(const ledc_channel_config_t *config) { (void)config; return ESP_OK; }
static inline esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t channel, uint32_t duty) { (void)mode; (void)channel; (void)duty; return ESP_OK; }
static inline esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t channel) { (void)mode; (void)channel; return ESP_OK; }
static inline void *heap_caps_malloc(size_t size, unsigned int caps) { (void)caps; return malloc(size); }
static inline void *heap_caps_calloc(size_t count, size_t size, unsigned int caps) { (void)caps; return calloc(count, size); }
static inline void heap_caps_free(void *ptr) { free(ptr); }
static inline size_t heap_caps_get_free_size(unsigned int caps) { (void)caps; return 64 * 1024 * 1024; }
static inline void heap_caps_print_heap_info(unsigned int caps) { (void)caps; }

#endif
