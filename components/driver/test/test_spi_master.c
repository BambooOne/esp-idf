/*
 Tests for the spi_master device driver
*/

#include <esp_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/xtensa_api.h"
#include "unity.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "soc/dport_reg.h"
#include "soc/spi_reg.h"
#include "soc/spi_struct.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/ringbuf.h"


static void check_spi_pre_n_for(int clk, int pre, int n)
{
    esp_err_t ret;
    spi_device_handle_t handle;

    spi_device_interface_config_t devcfg={
        .command_bits=0,
        .address_bits=0,
        .dummy_bits=0,
        .clock_speed_hz=clk,
        .duty_cycle_pos=128,
        .mode=0,
        .spics_io_num=21,
        .queue_size=3
    };
    char sendbuf[16]="";
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    ret=spi_bus_add_device(HSPI_HOST, &devcfg, &handle);
    TEST_ASSERT(ret==ESP_OK);

    t.length=16*8;
    t.tx_buffer=sendbuf;
    ret=spi_device_transmit(handle, &t);

    printf("Checking clk rate %dHz. expect pre %d n %d, got pre %d n %d\n", clk, pre, n, SPI2.clock.clkdiv_pre+1, SPI2.clock.clkcnt_n+1);

    TEST_ASSERT(SPI2.clock.clkcnt_n+1==n);
    TEST_ASSERT(SPI2.clock.clkdiv_pre+1==pre);

    ret=spi_bus_remove_device(handle);
    TEST_ASSERT(ret==ESP_OK);
}


TEST_CASE("SPI Master clockdiv calculation routines", "[spi]")
{
    spi_bus_config_t buscfg={
        .mosi_io_num=4,
        .miso_io_num=26,
        .sclk_io_num=25,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1
    };
    esp_err_t ret;
    ret=spi_bus_initialize(HSPI_HOST, &buscfg, 1);
    TEST_ASSERT(ret==ESP_OK);

    check_spi_pre_n_for(26000000, 1, 3);
    check_spi_pre_n_for(20000000, 1, 4);
    check_spi_pre_n_for(8000000, 1, 10);
    check_spi_pre_n_for(800000, 2, 50);
    check_spi_pre_n_for(100000, 16, 50);
    check_spi_pre_n_for(333333, 4, 60);
    check_spi_pre_n_for(900000, 2, 44);
    check_spi_pre_n_for(1, 8192, 64); //Actually should generate the minimum clock speed, 152Hz
    check_spi_pre_n_for(26000000, 1, 3);

    ret=spi_bus_free(HSPI_HOST);
    TEST_ASSERT(ret==ESP_OK);
}

static spi_device_handle_t setup_spi_bus(int clkspeed, bool dma) {
    spi_bus_config_t buscfg={
        .mosi_io_num=4,
        .miso_io_num=26,
        .sclk_io_num=25,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz=4096*3
    };
    spi_device_interface_config_t devcfg={
        .command_bits=0,
        .address_bits=0,
        .dummy_bits=0,
        .clock_speed_hz=clkspeed,
        .duty_cycle_pos=128,
        .mode=0,
        .spics_io_num=21,
        .queue_size=3,
    };
    esp_err_t ret;
    spi_device_handle_t handle;
    printf("THIS TEST NEEDS A JUMPER BETWEEN IO4 AND IO26\n");

    ret=spi_bus_initialize(HSPI_HOST, &buscfg, dma?1:0);
    TEST_ASSERT(ret==ESP_OK);
    ret=spi_bus_add_device(HSPI_HOST, &devcfg, &handle);
    TEST_ASSERT(ret==ESP_OK);
    printf("Bus/dev inited.\n");
    return handle;
}

static void spi_test(spi_device_handle_t handle, int num_bytes) {
    esp_err_t ret;
    int x;
    srand(num_bytes);
    char *sendbuf=heap_caps_malloc(num_bytes, MALLOC_CAP_DMA);
    char *recvbuf=heap_caps_malloc(num_bytes, MALLOC_CAP_DMA);
    for (x=0; x<num_bytes; x++) {
        sendbuf[x]=rand()&0xff;
        recvbuf[x]=0x55;
    }

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length=num_bytes*8;
    t.tx_buffer=sendbuf;
    t.rx_buffer=recvbuf;
    t.addr=0xA00000000000000FL;
    t.cmd=0x55;

    printf("Transmitting %d bytes...\n", num_bytes);
    ret=spi_device_transmit(handle, &t);
    TEST_ASSERT(ret==ESP_OK);

    srand(num_bytes);
    for (x=0; x<num_bytes; x++) {
        if (sendbuf[x]!=(rand()&0xff)) {
            printf("Huh? Sendbuf corrupted at byte %d\n", x);
            TEST_ASSERT(0);
        }
        if (sendbuf[x]!=recvbuf[x]) break;
    }
    if (x!=num_bytes) {
        int from=x-16;
        if (from<0) from=0;
        printf("Error at %d! Sent vs recved: (starting from %d)\n" , x, from);
        for (int i=0; i<32; i++) {
            if (i+from<num_bytes) printf("%02X ", sendbuf[from+i]);
        }
        printf("\n");
        for (int i=0; i<32; i++) {
            if (i+from<num_bytes) printf("%02X ", recvbuf[from+i]);
        }
        printf("\n");
//        TEST_ASSERT(0);
    }

    printf("Success!\n");

    free(sendbuf);
    free(recvbuf);
}

static void destroy_spi_bus(spi_device_handle_t handle) {
    esp_err_t ret;
    ret=spi_bus_remove_device(handle);
    TEST_ASSERT(ret==ESP_OK);
    ret=spi_bus_free(HSPI_HOST);
    TEST_ASSERT(ret==ESP_OK);
}


#define TEST_LEN 111

TEST_CASE("SPI Master test", "[spi][ignore]")
{
    printf("Testing bus at 80KHz\n");
    spi_device_handle_t handle=setup_spi_bus(80000, true);
    spi_test(handle, 16); //small
    spi_test(handle, 21); //small, unaligned
    spi_test(handle, 36); //aligned
    spi_test(handle, 128); //aligned
    spi_test(handle, 129); //unaligned
    spi_test(handle, 4096-2); //multiple descs, edge case 1
    spi_test(handle, 4096-1); //multiple descs, edge case 2
    spi_test(handle, 4096*3); //multiple descs

    destroy_spi_bus(handle);

    printf("Testing bus at 80KHz, non-DMA\n");
    handle=setup_spi_bus(80000, false);
    spi_test(handle, 4); //aligned
    spi_test(handle, 16); //small
    spi_test(handle, 21); //small, unaligned

    destroy_spi_bus(handle);


    printf("Testing bus at 26MHz\n");
    handle=setup_spi_bus(20000000, true);

    spi_test(handle, 128); //DMA, aligned
    spi_test(handle, 4096*3); //DMA, multiple descs
    destroy_spi_bus(handle);

    printf("Testing bus at 900KHz\n");
    handle=setup_spi_bus(9000000, true);

    spi_test(handle, 128); //DMA, aligned
    spi_test(handle, 4096*3); //DMA, multiple descs
    destroy_spi_bus(handle);
}


TEST_CASE("SPI Master test, interaction of multiple devs", "[spi][ignore]") {
    esp_err_t ret;
    spi_device_interface_config_t devcfg={
        .command_bits=0,
        .address_bits=0,
        .dummy_bits=0,
        .clock_speed_hz=1000000,
        .duty_cycle_pos=128,
        .mode=0,
        .spics_io_num=23,
        .queue_size=3,
    };
    spi_device_handle_t handle1=setup_spi_bus(80000, true);
    spi_device_handle_t handle2;
    spi_bus_add_device(HSPI_HOST, &devcfg, &handle2);

    printf("Sending to dev 1\n");
    spi_test(handle1, 7);
    printf("Sending to dev 1\n");
    spi_test(handle1, 15);
    printf("Sending to dev 2\n");
    spi_test(handle2, 15);
    printf("Sending to dev 1\n");
    spi_test(handle1, 32);
    printf("Sending to dev 2\n");
    spi_test(handle2, 32);
    printf("Sending to dev 1\n");
    spi_test(handle1, 63);
    printf("Sending to dev 2\n");
    spi_test(handle2, 63);
    printf("Sending to dev 1\n");
    spi_test(handle1, 5000);
    printf("Sending to dev 2\n");
    spi_test(handle2, 5000);


    ret=spi_bus_remove_device(handle2);
    TEST_ASSERT(ret==ESP_OK);
    destroy_spi_bus(handle1);
}

TEST_CASE("SPI Master no response when switch from host1 (HSPI) to host2 (VSPI)", "[spi]")
{
    //spi config
    spi_bus_config_t bus_config;
    spi_device_interface_config_t device_config;
    spi_device_handle_t spi;
    spi_host_device_t host;
    int dma = 1;

    memset(&bus_config, 0, sizeof(spi_bus_config_t));
    memset(&device_config, 0, sizeof(spi_device_interface_config_t));

    bus_config.miso_io_num = -1;
    bus_config.mosi_io_num = 26;
    bus_config.sclk_io_num = 25;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;

    device_config.clock_speed_hz = 50000;
    device_config.mode = 0;
    device_config.spics_io_num = -1;
    device_config.queue_size = 1;
    device_config.flags = SPI_DEVICE_TXBIT_LSBFIRST | SPI_DEVICE_RXBIT_LSBFIRST;

    struct spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 16,
        .rx_buffer = NULL,
        .tx_data = {0x04, 0x00}
    };

    //initialize for first host
    host = 1;

    TEST_ASSERT(spi_bus_initialize(host, &bus_config, dma) == ESP_OK);
    TEST_ASSERT(spi_bus_add_device(host, &device_config, &spi) == ESP_OK);

    printf("before first xmit\n");
    TEST_ASSERT(spi_device_transmit(spi, &transaction) == ESP_OK);
    printf("after first xmit\n");

    TEST_ASSERT(spi_bus_remove_device(spi) == ESP_OK);
    TEST_ASSERT(spi_bus_free(host) == ESP_OK);

    //for second host and failed before
    host = 2;

    TEST_ASSERT(spi_bus_initialize(host, &bus_config, dma) == ESP_OK);
    TEST_ASSERT(spi_bus_add_device(host, &device_config, &spi) == ESP_OK);

    printf("before second xmit\n");
    // the original version (bit mis-written) stucks here.
    TEST_ASSERT(spi_device_transmit(spi, &transaction) == ESP_OK);
    // test case success when see this.
    printf("after second xmit\n");

    TEST_ASSERT(spi_bus_remove_device(spi) == ESP_OK);
    TEST_ASSERT(spi_bus_free(host) == ESP_OK);
}

IRAM_ATTR  static uint32_t data_iram[320];
DRAM_ATTR  static uint32_t data_dram[320];
//force to place in code area.
static const uint32_t data_drom[320] = {0};

#define PIN_NUM_MISO 25
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  19
#define PIN_NUM_CS   22

#define PIN_NUM_DC   21
#define PIN_NUM_RST  18
#define PIN_NUM_BCKL 5

TEST_CASE("SPI Master DMA test, TX and RX in different regions", "[spi]")
{
    uint32_t data_rxdram[320];

    esp_err_t ret;
    spi_device_handle_t spi;
    spi_bus_config_t buscfg={
        .miso_io_num=PIN_NUM_MISO,
        .mosi_io_num=PIN_NUM_MOSI,
        .sclk_io_num=PIN_NUM_CLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1
    };
    spi_device_interface_config_t devcfg={
        .clock_speed_hz=10000000,               //Clock out at 10 MHz
        .mode=0,                                //SPI mode 0
        .spics_io_num=PIN_NUM_CS,               //CS pin
        .queue_size=7,                          //We want to be able to queue 7 transactions at a time
        .pre_cb=NULL,  //Specify pre-transfer callback to handle D/C line
    };
    //Initialize the SPI bus
    ret=spi_bus_initialize(HSPI_HOST, &buscfg, 1);
    TEST_ASSERT(ret==ESP_OK);
    //Attach the LCD to the SPI bus
    ret=spi_bus_add_device(HSPI_HOST, &devcfg, &spi);
    TEST_ASSERT(ret==ESP_OK);

    static spi_transaction_t trans[6];
    int x;

    printf("iram: %p, dram: %p, drom: %p\n", data_iram, data_dram, data_drom);

    memset(trans, 0, 6*sizeof(spi_transaction_t));

    trans[0].length = 320*8,
    trans[0].tx_buffer = data_iram;
    trans[0].rx_buffer = data_rxdram;

    trans[1].length = 320*8,
    trans[1].tx_buffer = data_dram;
    trans[1].rx_buffer = data_rxdram;

    trans[2].length = 320*8,
    trans[2].tx_buffer = data_drom;
    trans[2].rx_buffer = data_rxdram;

    trans[3].length = 320*8,
    trans[3].tx_buffer = data_drom;
    trans[3].rx_buffer = data_iram;

    trans[4].length = 320*8,
    trans[4].rxlength = 8*4;
    trans[4].tx_buffer = data_drom;
    trans[4].flags = SPI_TRANS_USE_RXDATA;

    trans[5].length = 8*4;
    trans[5].flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA;

    //Queue all transactions.
    for (x=0; x<6; x++) {
        ret=spi_device_queue_trans(spi,&trans[x], portMAX_DELAY);
        TEST_ASSERT(ret==ESP_OK);
    }

    for (x=0; x<6; x++) {
        spi_transaction_t* ptr;
        ret=spi_device_get_trans_result(spi,&ptr, portMAX_DELAY);
        TEST_ASSERT(ret==ESP_OK);
        TEST_ASSERT(ptr = trans+x);
    }

    TEST_ASSERT(spi_bus_remove_device(spi) == ESP_OK);
    TEST_ASSERT(spi_bus_free(HSPI_HOST) == ESP_OK);
}


static inline void int_connect( uint32_t gpio, uint32_t sigo, uint32_t sigi )
{
    gpio_matrix_out( gpio, sigo, false, false );
    gpio_matrix_in( gpio, sigi, false );
}

//this part tests 3 DMA issues in master mode, full-duplex in IDF2.1
// 1. RX buffer not aligned (start and end)
// 2. not setting rx_buffer
// 3. setting rx_length != length
TEST_CASE("SPI Master DMA test: length, start, not aligned", "[spi]")
{
    uint8_t tx_buf[320]={0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0xaa, 0xcc, 0xff, 0xee, 0x55, 0x77, 0x88, 0x43};
    uint8_t rx_buf[320];

    esp_err_t ret;
    spi_device_handle_t spi;
    spi_bus_config_t buscfg={
        .miso_io_num=PIN_NUM_MISO,
        .mosi_io_num=PIN_NUM_MOSI,
        .sclk_io_num=PIN_NUM_CLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1
    };
    spi_device_interface_config_t devcfg={
        .clock_speed_hz=10*1000*1000,               //Clock out at 10 MHz
        .mode=0,                                //SPI mode 0
        .spics_io_num=PIN_NUM_CS,               //CS pin
        .queue_size=7,                          //We want to be able to queue 7 transactions at a time
        .pre_cb=NULL,
    };
    //Initialize the SPI bus
    ret=spi_bus_initialize(HSPI_HOST, &buscfg, 1);
    TEST_ASSERT(ret==ESP_OK);
    //Attach the LCD to the SPI bus
    ret=spi_bus_add_device(HSPI_HOST, &devcfg, &spi);
    TEST_ASSERT(ret==ESP_OK);

    //do internal connection
    int_connect( PIN_NUM_MOSI, HSPID_OUT_IDX, HSPIQ_IN_IDX );

    memset(rx_buf, 0x66, 320);

    for ( int i = 0; i < 8; i ++ ) {
        memset( rx_buf, 0x66, sizeof(rx_buf));

        spi_transaction_t t = {};
        t.length = 8*(i+1);
        t.rxlength = 0;
        t.tx_buffer = tx_buf+2*i;
        t.rx_buffer = rx_buf + i;

        if ( i == 1 ) {
            //test set no start
            t.rx_buffer = NULL;
        } else if ( i == 2 ) {
            //test rx length != tx_length
            t.rxlength = t.length - 8;
        }
        spi_device_transmit( spi, &t );

        for( int i = 0; i < 16; i ++ ) {
            printf("%02X ", rx_buf[i]);
        }
        printf("\n");

        if ( i == 1 ) {
            // no rx, skip check
        } else if ( i == 2 ) {
            //test rx length = tx length-1
            TEST_ASSERT( memcmp(t.tx_buffer, t.rx_buffer, t.length/8-1)==0 );
        } else {
            //normal check
            TEST_ASSERT( memcmp(t.tx_buffer, t.rx_buffer, t.length/8)==0 );
        }
    }

    TEST_ASSERT(spi_bus_remove_device(spi) == ESP_OK);
    TEST_ASSERT(spi_bus_free(HSPI_HOST) == ESP_OK);
}

static const char MASTER_TAG[] = "test_master";
static const char SLAVE_TAG[] = "test_slave";
//DRAM_ATTR static uint8_t master_send[] = {0x93, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0xaa, 0xcc, 0xff, 0xee, 0x55, 0x77, 0x88, 0x43};
DRAM_ATTR static uint8_t slave_send[] = { 0xaa, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10, 0x13, 0x57, 0x9b, 0xdf, 0x24, 0x68, 0xac, 0xe0 };

/*
static void master_init( spi_device_handle_t* spi, int mode, uint32_t speed)
{
    esp_err_t ret;
    spi_bus_config_t buscfg={
        .miso_io_num=PIN_NUM_MISO,
        .mosi_io_num=PIN_NUM_MOSI,
        .sclk_io_num=PIN_NUM_CLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1
    };
    spi_device_interface_config_t devcfg={
        .clock_speed_hz=speed,            //currently only up to 4MHz for internel connect
        .mode=mode,                                //SPI mode 0
        .spics_io_num=PIN_NUM_CS,               //CS pin
        .queue_size=16,                          //We want to be able to queue 7 transactions at a time
        .pre_cb=NULL,
        .cs_ena_pretrans = 0,
    };
    //Initialize the SPI bus
    ret=spi_bus_initialize(HSPI_HOST, &buscfg, 1);
    TEST_ASSERT(ret==ESP_OK);
    //Attach the LCD to the SPI bus
    ret=spi_bus_add_device(HSPI_HOST, &devcfg, spi);
    TEST_ASSERT(ret==ESP_OK);
}

static void slave_init(int mode, int dma_chan)
{
    //Configuration for the SPI bus
    spi_bus_config_t buscfg={
        .mosi_io_num=PIN_NUM_MOSI,
        .miso_io_num=PIN_NUM_MISO,
        .sclk_io_num=PIN_NUM_CLK
    };
    //Configuration for the SPI slave interface
    spi_slave_interface_config_t slvcfg={
        .mode=mode,
        .spics_io_num=PIN_NUM_CS,
        .queue_size=3,
        .flags=0,
    };
    //Enable pull-ups on SPI lines so we don't detect rogue pulses when no master is connected.
    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS, GPIO_PULLUP_ONLY);
    //Initialize SPI slave interface
    TEST_ESP_OK( spi_slave_initialize(VSPI_HOST, &buscfg, &slvcfg, dma_chan) );
}
*/

typedef struct {
    uint32_t len;
    uint8_t *start;
} slave_txdata_t;

typedef struct {
    uint32_t len;
    uint8_t data[1];
} slave_rxdata_t;

typedef struct {
    RingbufHandle_t data_received;
    QueueHandle_t data_to_send;
} spi_slave_task_context_t;

esp_err_t init_slave_context(spi_slave_task_context_t *context)
{
    context->data_to_send = xQueueCreate( 16, sizeof( slave_txdata_t ));
    if ( context->data_to_send == NULL ) {
        return ESP_ERR_NO_MEM;
    }
    context->data_received = xRingbufferCreate( 1024, RINGBUF_TYPE_NOSPLIT );
    if ( context->data_received == NULL ) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void deinit_slave_context(spi_slave_task_context_t *context)
{
    TEST_ASSERT( context->data_to_send != NULL );
    vQueueDelete( context->data_to_send );
    context->data_to_send = NULL;
    TEST_ASSERT( context->data_received != NULL );
    vRingbufferDelete( context->data_received );
    context->data_received = NULL;
}

static void task_slave(void* arg)
{
    spi_slave_task_context_t* context = (spi_slave_task_context_t*) arg;
    QueueHandle_t queue = context->data_to_send;
    RingbufHandle_t ringbuf = context->data_received;
    uint8_t recvbuf[320+4];
    slave_txdata_t txdata;

    ESP_LOGI( SLAVE_TAG, "slave up" );
    //never quit, but blocked by the queue, waiting to be killed, when no more send from main task.
    while( 1 ) {
        xQueueReceive( queue, &txdata, portMAX_DELAY );

        ESP_LOGI( "test", "received: %p", txdata.start );
        spi_slave_transaction_t t = {};
        t.length = txdata.len;
        t.tx_buffer = txdata.start;
        t.rx_buffer = recvbuf+4;
        //loop until trans_len != 0 to skip glitches
        memset(recvbuf, 0x66, sizeof(recvbuf));
        do {
            TEST_ESP_OK( spi_slave_transmit( VSPI_HOST, &t, portMAX_DELAY ) );
        } while ( t.trans_len == 0 );
        *(uint32_t*)recvbuf = t.trans_len;
        ESP_LOGI( SLAVE_TAG, "received: %d", t.trans_len );
        xRingbufferSend( ringbuf, recvbuf, 4+(t.trans_len+7)/8, portMAX_DELAY );
    }
}

#define TEST_SPI_HOST   HSPI_HOST
#define TEST_SLAVE_HOST VSPI_HOST

static uint8_t bitswap(uint8_t in)
{
    uint8_t out = 0;
    for (int i = 0; i < 8; i++) {
        out = out >> 1;
        if (in&0x80) out |= 0x80;
        in = in << 1;
    }
    return out;
}

#define SPI_BUS_TEST_DEFAULT_CONFIG() {\
        .miso_io_num=PIN_NUM_MISO, \
        .mosi_io_num=PIN_NUM_MOSI,\
        .sclk_io_num=PIN_NUM_CLK,\
        .quadwp_io_num=-1,\
        .quadhd_io_num=-1\
    }

#define SPI_DEVICE_TEST_DEFAULT_CONFIG()    {\
        .clock_speed_hz=10*1000*1000,\
        .mode=0,\
        .spics_io_num=PIN_NUM_CS,\
        .queue_size=16,\
        .pre_cb=NULL,  \
        .cs_ena_pretrans = 0,\
        .cs_ena_posttrans = 0,\
    }

#define SPI_SLAVE_TEST_DEFAULT_CONFIG() {\
        .mode=0,\
        .spics_io_num=PIN_NUM_CS,\
        .queue_size=3,\
        .flags=0,\
    }

void test_cmd_addr(spi_slave_task_context_t *slave_context, bool lsb_first)
{
    spi_device_handle_t spi;
    ESP_LOGI(MASTER_TAG, ">>>>>>>>> TEST %s FIRST <<<<<<<<<<<", lsb_first?"LSB":"MSB");

    //initial master, mode 0, 1MHz
    spi_bus_config_t buscfg=SPI_BUS_TEST_DEFAULT_CONFIG();
    TEST_ESP_OK(spi_bus_initialize(TEST_SPI_HOST, &buscfg, 1));
    spi_device_interface_config_t devcfg=SPI_DEVICE_TEST_DEFAULT_CONFIG();
    devcfg.clock_speed_hz = 1*1000*1000;
    if (lsb_first) devcfg.flags |= SPI_DEVICE_BIT_LSBFIRST;
    TEST_ESP_OK(spi_bus_add_device(TEST_SPI_HOST, &devcfg, &spi));

    //connecting pins to two peripherals breaks the output, fix it.
    int_connect( PIN_NUM_MOSI,  HSPID_OUT_IDX,   VSPIQ_IN_IDX );
    int_connect( PIN_NUM_MISO,  VSPIQ_OUT_IDX,   HSPID_IN_IDX );
    int_connect( PIN_NUM_CS,    HSPICS0_OUT_IDX, VSPICS0_IN_IDX );
    int_connect( PIN_NUM_CLK,   HSPICLK_OUT_IDX, VSPICLK_IN_IDX );

    for (int i= 0; i < 8; i++) {
        //prepare slave tx data
        slave_txdata_t slave_txdata = (slave_txdata_t) {
            .start = slave_send,
            .len = 256,
        };
        xQueueSend(slave_context->data_to_send, &slave_txdata, portMAX_DELAY);

        vTaskDelay(50);
        //prepare master tx data
        int cmd_bits = (i+1)*2;
        int addr_bits = 56-8*i;
        int round_up = (cmd_bits+addr_bits+7)/8*8;
        addr_bits = round_up - cmd_bits;

        spi_transaction_ext_t trans = (spi_transaction_ext_t) {
            .base = {
                .flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR,
                .addr = 0x456789abcdef0123,
                .cmd = 0xcdef,
            },
            .command_bits = cmd_bits,
            .address_bits = addr_bits,
        };

        ESP_LOGI( MASTER_TAG, "===== test%d =====", i );
        ESP_LOGI(MASTER_TAG, "cmd_bits： %d, addr_bits: %d", cmd_bits, addr_bits);
        TEST_ESP_OK(spi_device_transmit(spi, (spi_transaction_t*)&trans));
        //wait for both master and slave end

        size_t rcv_len;
        slave_rxdata_t *rcv_data = xRingbufferReceive(slave_context->data_received, &rcv_len, portMAX_DELAY);
        rcv_len-=4;
        uint8_t *buffer = rcv_data->data;

        ESP_LOGI(SLAVE_TAG, "trans_len: %d", rcv_len);
        TEST_ASSERT_EQUAL(rcv_len, (rcv_data->len+7)/8);
        TEST_ASSERT_EQUAL(rcv_data->len, cmd_bits+addr_bits);
        ESP_LOG_BUFFER_HEX("slave rx", buffer, rcv_len);

        uint16_t cmd_expected = trans.base.cmd & (BIT(cmd_bits) - 1);
        uint64_t addr_expected = trans.base.addr & ((1ULL<<addr_bits) - 1);

        uint8_t *data_ptr = buffer;
        uint16_t cmd_got = *(uint16_t*)data_ptr;
        data_ptr += cmd_bits/8;
        cmd_got = __builtin_bswap16(cmd_got);
        cmd_got = cmd_got >> (16-cmd_bits);
        int remain_bits = cmd_bits % 8;

        uint64_t addr_got = *(uint64_t*)data_ptr;
        data_ptr += 8;
        addr_got = __builtin_bswap64(addr_got);
        addr_got = (addr_got << remain_bits);
        addr_got |= (*data_ptr >> (8-remain_bits));
        addr_got = addr_got >> (64-addr_bits);

        if (lsb_first) {
            cmd_got = __builtin_bswap16(cmd_got);
            addr_got = __builtin_bswap64(addr_got);

            uint8_t *swap_ptr = (uint8_t*)&cmd_got;
            swap_ptr[0] = bitswap(swap_ptr[0]);
            swap_ptr[1] = bitswap(swap_ptr[1]);
            cmd_got = cmd_got >> (16-cmd_bits);

            swap_ptr = (uint8_t*)&addr_got;
            for (int j = 0; j < 8; j++) swap_ptr[j] = bitswap(swap_ptr[j]);
            addr_got = addr_got >> (64-addr_bits);
        }

        ESP_LOGI(SLAVE_TAG, "cmd_got: %04X, addr_got: %08X%08X", cmd_got, (uint32_t)(addr_got>>32), (uint32_t)addr_got);

        TEST_ASSERT_EQUAL_HEX16(cmd_expected, cmd_got);
        if (addr_bits > 0) {
            TEST_ASSERT_EQUAL_HEX32(addr_expected, addr_got);
            TEST_ASSERT_EQUAL_HEX32(addr_expected >> 8, addr_got >> 8);
        }

        //clean
        vRingbufferReturnItem(slave_context->data_received, buffer);
    }

    TEST_ASSERT(spi_bus_remove_device(spi) == ESP_OK);
    TEST_ASSERT(spi_bus_free(TEST_SPI_HOST) == ESP_OK);
}

TEST_CASE("SPI master variable cmd & addr test","[spi]")
{
    spi_slave_task_context_t slave_context = {};
    esp_err_t err = init_slave_context( &slave_context );
    TEST_ASSERT( err == ESP_OK );
    TaskHandle_t handle_slave;
    xTaskCreate( task_slave, "spi_slave", 4096, &slave_context, 0, &handle_slave);

    //initial slave, mode 0, no dma
    int dma_chan = 0;
    int slave_mode = 0;
    spi_bus_config_t slv_buscfg=SPI_BUS_TEST_DEFAULT_CONFIG();
    spi_slave_interface_config_t slvcfg=SPI_SLAVE_TEST_DEFAULT_CONFIG();
    slvcfg.mode = slave_mode;
    //Initialize SPI slave interface
    TEST_ESP_OK( spi_slave_initialize(TEST_SLAVE_HOST, &slv_buscfg, &slvcfg, dma_chan) );

    test_cmd_addr(&slave_context, false);
    test_cmd_addr(&slave_context, true);

    vTaskDelete( handle_slave );
    handle_slave = 0;

    deinit_slave_context(&slave_context);

    TEST_ASSERT(spi_slave_free(TEST_SLAVE_HOST) == ESP_OK);

    ESP_LOGI(MASTER_TAG, "test passed.");
}
